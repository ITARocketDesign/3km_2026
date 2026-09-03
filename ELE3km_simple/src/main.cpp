// ELE3km_simple — firmware de último recurso.
//
// Issue 02: o esqueleto de voo. Um único laço a 50 Hz, com a ordem de boot segura
// e o watchdog, mas ainda SEM lógica de sensor no ar — sem altitude, sem pacote,
// sem gravação por ciclo. É a espinha em que as fatias seguintes penduram os
// sensores (issues 03–05), o cartão (06) e o resto.
//
// A diferença de fundo para o ELE3km: aqui NÃO há duas tasks nem ring buffer. É um
// superloop único (PRD → arquitetura). Com uma thread só, a regra de arbitragem do
// barramento SPI deixa de existir: código sequencial nunca tem dois mestres SPI ao
// mesmo tempo. A proteção contra travada dura do cartão passa a ser reboot pelo
// watchdog, não isolamento entre cores.
//
// O superloop roda no próprio loop() do Arduino (a loopTask), que é quem se inscreve
// na TWDT e a alimenta a cada volta. Ver DISCIPLINE.md regra 1 antes de tocar em
// GPIO25 (o habilitador de TX, não um indicador luminoso): nenhum código o pisca.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#include "esp_task_wdt.h"

#include "core/log_codec.h"
#include "core/survival_computer.h"
#include "core/telemetry_codec.h"
#include "core/types.h"
#include "hal/board.h"
#include "hal/bmp280.h"
#include "hal/boot_counter.h"
#include "hal/gps_neo6m.h"
#include "hal/i2c_bus.h"
#include "hal/mpu6050.h"
#include "hal/radio_sx1276.h"
#include "hal/sd_log.h"
#include "pins.h"

namespace {

// 50 Hz (Δt = 20 ms): a cadência de projeto do superloop (PRD → arquitetura). O
// barômetro é lido a 25 Hz num subciclo; a IMU e a drenagem da UART do GPS, a cada
// volta.
constexpr uint32_t kCyclePeriodMs = 20;
constexpr uint32_t kBaroPeriodMs = 40;  // 25 Hz

// Despejo de diagnóstico dos sensores no Serial durante teste de bancada. A 50 Hz
// encheria o buffer da UART; a alguns hertz é inofensiva. Ponha 0 para desligar.
constexpr uint32_t kSensorPrintPeriodMs = 250;

// ── HAL da placa ────────────────────────────────────────────────────────────
// Uma thread só: todos os módulos são tocados pelo superloop. Sensores por I²C e
// UART; rádio e cartão por SPI. Não há estado compartilhado entre cores porque
// não há um segundo core em uso.
hal::Bmp280      g_baro(Wire);
hal::Mpu6050     g_imu(Wire);
hal::GpsNeo6m    g_gps(Serial1);
hal::RadioSx1276 g_sx1276;
hal::SdLog       g_log;
hal::BootCounter g_boot;

// Contador de boot deste voo: índice do nome do arquivo E número gravado em cada
// registro de 64 B (issue 06). Lido uma vez no setup, usado no laço.
uint16_t g_boot_count = 0;

// O núcleo puro: monta o registro (todo ciclo) e o pacote (a 1 Hz). Sem estado
// global — a instância mora aqui, na HAL.
core::SurvivalComputer g_computer;

// Resultado do boot. Um módulo ausente marca sua flag como falsa e o boot segue —
// nenhum begin() que falha trava o setup().
bool g_baro_ok   = false;
bool g_imu_ok    = false;
bool g_gps_ok    = false;
bool g_sx1276_ok = false;
bool g_sd_ok     = false;

// Despeja a amostra de sensores crua e a altitude derivada no Serial. Só para
// bancada: mostra o que baro, IMU e GPS estão entregando e a pressão-altitude que
// o núcleo derivou contra o datum fixo. O '--' marca leitura não confiável nesta
// amostra; 'SAT' marca acelerômetro no fundo de escala.
void print_sensor_sample(const core::SensorSample& s, const core::LogRecord& r) {
    Serial.printf("baro: %s P=%.1f Pa  T=%.2f C  alt=%.1f m (datum %.0f Pa)\n",
                  s.baro_valid ? "ok" : "--", s.pressure_pa, s.temperature_c,
                  r.altitude_m, core::kFixedDatumPa);
    Serial.printf("imu:  %s%s a=[%d %d %d] mg  g=[%d %d %d] ddps\n",
                  s.imu_valid ? "ok" : "--", s.accel_saturated ? " SAT" : "",
                  s.accel_mg[0], s.accel_mg[1], s.accel_mg[2],
                  s.gyro_ddps[0], s.gyro_ddps[1], s.gyro_ddps[2]);
    Serial.printf("gps:  recv=%d fix=%d q=%u sats=%u HDOP=%.1f lat=%.7f lon=%.7f\n",
                  s.gps.receiving, s.gps.valid, s.gps.fix_quality, s.gps.satellites,
                  s.gps.hdop_half * 0.5f, s.gps.latitude_1e7 / 1e7,
                  s.gps.longitude_1e7 / 1e7);
}

}  // namespace

void setup() {
    // A PRIMEIRA coisa: chip-selects em nível seguro (GPIO18/23/32 HIGH), o E22
    // abandonado preso em reset, e WiFi/Bluetooth desligados. Tudo antes de
    // SPI.begin() — a ordem é obrigatória (DISCIPLINE.md) e mora aqui.
    hal::board_early_init();

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ELE3km_simple — issue 07: recuperacao minima do barramento I2C (clock-out + begin, 1/ciclo)");

    // Só agora o barramento SPI pode subir. Um rádio e o cartão dividem esta VSPI.
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_LORA_CS);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

    g_baro_ok = g_baro.begin();
    Serial.printf("BMP280: %s", g_baro_ok ? "ok" : "AUSENTE");
    if (g_baro_ok) {
        Serial.printf(" (0x%02X)", g_baro.address());
    }
    Serial.println();

    g_imu_ok = g_imu.begin();
    Serial.printf("MPU6050: %s", g_imu_ok ? "ok, +-16 g / +-2000 dps" : "AUSENTE");
    if (g_imu_ok) {
        Serial.printf(" (0x%02X)", g_imu.address());
    }
    Serial.println();

    g_gps_ok = g_gps.begin(millis());
    Serial.printf("NEO-6M: %s\n", g_gps_ok ? "UART aberta, Airborne <4g enviado" : "FALHOU");

    // ⚠️ Nunca ligar a placa sem a antena do SX1276 conectada: o firmware não
    // detecta antena solta, e transmitir em circuito aberto danifica o PA. begin()
    // apenas configura o rádio — não transmite; a TX a 1 Hz entra na issue 03.
    g_sx1276_ok = g_sx1276.begin();
    Serial.printf("SX1276: %s\n", g_sx1276_ok ? "ok, 915 MHz, SF7" : "FALHOU");

    // Contador de boot persistente (NVS): o índice do arquivo deste voo e o número
    // gravado em cada registro. Lido antes de abrir o cartão — o nome do arquivo e o
    // conteúdo dele têm de concordar sobre a qual voo pertencem (issue 06).
    g_boot_count = g_boot.next();
    Serial.printf("boot #%u\n", g_boot_count);

    // Cartão: cria o arquivo pré-alocado e contíguo deste boot e escreve o bloco de
    // cabeçalho com o datum fixo (issue 06). begin() já monta o cartão; a escrita
    // por ciclo entra no superloop. Um cartão ausente marca a flag e o boot segue.
    g_sd_ok = g_log.begin(g_boot_count, core::kFixedDatumPa);
    Serial.printf("microSD: %s\n",
                  g_sd_ok ? "arquivo pre-alocado, cabecalho gravado" : "AUSENTE/FALHOU");

    // A partir daqui a loopTask precisa alimentar o watchdog a cada volta. A TWDT
    // do framework (arduino-esp32 2.0.17 / IDF 4.4) tem timeout global de ~5 s; um
    // deadlock real no superloop vira reset limpo, que reentra no boot.
    esp_task_wdt_add(nullptr);

    Serial.printf("superloop a %u Hz (Dt = %u ms); watchdog inscrito\n",
                  1000u / kCyclePeriodMs, kCyclePeriodMs);
}

// O superloop de 50 Hz. Por enquanto: alimenta o watchdog, drena a UART do GPS, lê
// os sensores e imprime um diagnóstico a alguns hertz. Nenhuma lógica de voo ainda.
void loop() {
    static TickType_t last_wake = xTaskGetTickCount();
    static uint32_t next_baro_ms = 0;
    static uint32_t next_print_ms = 0;

    const uint32_t cycle_start_ms = millis();

    // 1. Alimenta o watchdog uma vez por volta.
    esp_task_wdt_reset();

    // 2. Drena a UART do GPS e, a cada ~10 s, reenvia a config UBX inteira
    //    (idempotente, sem detecção de reset — issue 05). O buffer tem ~530 ms de
    //    fôlego; drenar a cada volta o mantém longe do transbordo.
    if (g_gps_ok) {
        g_gps.service(cycle_start_ms);
    }

    // 3. Lê os sensores para a amostra de diagnóstico. IMU a 50 Hz, baro a 25 Hz num
    //    subciclo. Uma leitura I²C que falha depois do timeout duro do adaptador é um
    //    barramento travado (H5): um escravo segurando SDA baixa, que nenhum retry por
    //    software solta. A recuperação da issue 07 responde com clock-out por bit-bang
    //    (hal::i2c_bus_recover) + begin() do dispositivo, para soltar a linha e
    //    reaplicar timeout e configuração. No MÁXIMO um módulo é recuperado por ciclo
    //    — a rotina custa ~10–50 ms e não pode comer o ciclo de forma sustentada; o
    //    orçamento vai para o primeiro que falha (IMU antes do baro). Se o begin()
    //    falhar, a peça sumiu: a flag latcheia falsa e o módulo para de ser lido, e o
    //    watchdog/reboot é o backstop. Sem contador de reinit, sem retentativa de 5 s.
    core::SensorSample sample;  // defaults declaram "sem leitura fresca"
    bool i2c_recovered = false;  // orçamento de uma recuperação por ciclo

    if (g_imu_ok) {
        sample.imu_valid =
            g_imu.read(sample.accel_mg, sample.gyro_ddps, sample.accel_saturated);
        if (!sample.imu_valid && !i2c_recovered) {
            i2c_recovered = true;
            hal::i2c_bus_recover();
            g_imu_ok = g_imu.begin();
            Serial.printf("i2c: recover MPU6050 @ %lu ms -> %s\n",
                          static_cast<unsigned long>(cycle_start_ms),
                          g_imu_ok ? "ok" : "AUSENTE");
        }
    }
    if (g_baro_ok && static_cast<int32_t>(cycle_start_ms - next_baro_ms) >= 0) {
        next_baro_ms      = cycle_start_ms + kBaroPeriodMs;
        sample.baro_valid = g_baro.read(sample.pressure_pa, sample.temperature_c);
        if (!sample.baro_valid && !i2c_recovered) {
            i2c_recovered = true;
            hal::i2c_bus_recover();
            g_baro_ok = g_baro.begin();
            Serial.printf("i2c: recover BMP280 @ %lu ms -> %s\n",
                          static_cast<unsigned long>(cycle_start_ms),
                          g_baro_ok ? "ok" : "AUSENTE");
        }
    }
    if (g_gps_ok) {
        sample.gps                = g_gps.fix(cycle_start_ms);
        sample.gps_uart_overflows = g_gps.uart_overflow_count();
    }

    // 4. Núcleo: monta o registro (todo ciclo) e o pacote (a 1 Hz).
    const core::Outputs out = g_computer.update(sample, cycle_start_ms);

    // 4b. Cartão (issue 06): grava o registro deste ciclo no log durável. O núcleo
    //     monta um registro por ciclo; aqui ele é serializado com o contador de boot
    //     e empilhado no bloco de 512 B. Quando o bloco enche (8 registros), ele é
    //     gravado — a única chamada que toca o cartão no voo, sequencial com a TX.
    //     Se o cartão adoecer (travadas seguidas) ou o arquivo pré-alocado encher,
    //     is_open() cai e o superloop segue transmitindo sem gravar.
    if (g_log.is_open()) {
        uint8_t rec[core::kLogRecordSize];
        core::encode_record(out.record, g_boot_count, rec, sizeof(rec));
        g_log.stage(rec);
        if (g_log.block_ready()) {
            g_log.service(cycle_start_ms);
        }
    }

    // 5. Rádio. service() confirma o fim da TX anterior contra o registrador de
    //    status e aplica o timeout — a cada volta. Quando o núcleo libera um pacote,
    //    ele é codificado e disparado. Sequencial com tudo: uma thread só, sem regra
    //    de arbitragem de barramento.
    g_sx1276.service(cycle_start_ms);
    if (out.has_packet && g_sx1276_ok) {
        uint8_t  payload[core::kMaxPacketSize];
        const size_t len = core::encode_packet(out.packet, payload, sizeof(payload));
        if (len > 0) {
            g_sx1276.start_send(payload, len, cycle_start_ms);
        }
    }

    // 6. Diagnóstico a alguns hertz, com a folga do ciclo medida no Serial (o corpo
    //    do laço contra o alvo de 20 ms) — o critério de aceitação da issue.
    if (kSensorPrintPeriodMs > 0 &&
        static_cast<int32_t>(cycle_start_ms - next_print_ms) >= 0) {
        next_print_ms                = cycle_start_ms + kSensorPrintPeriodMs;
        const uint32_t body_ms       = millis() - cycle_start_ms;
        print_sensor_sample(sample, out.record);
        if (out.has_packet) {
            const bool full = out.packet.form == core::PacketForm::Full;
            Serial.printf("tx:   pacote #%u, %s (%u B, fonte=%u), alt=%d m, vz=%d dm/s, t=%u ds\n",
                          out.packet.sequence, full ? "completo" : "so-altitude",
                          static_cast<unsigned>(full ? core::kFullPacketSize
                                                      : core::kAltitudePacketSize),
                          static_cast<unsigned>(out.packet.position_source),
                          out.packet.altitude_m, out.packet.vertical_speed_dms,
                          out.packet.t_ds);
        }
        Serial.printf("loop: corpo=%lu ms  folga=%ld ms (alvo %lu ms)\n\n",
                      static_cast<unsigned long>(body_ms),
                      static_cast<long>(kCyclePeriodMs) - static_cast<long>(body_ms),
                      static_cast<unsigned long>(kCyclePeriodMs));
    }

    // Temporização a 50 Hz. vTaskDelayUntil ancora no despertar anterior, então a
    // cadência não deriva com a duração variável do corpo.
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCyclePeriodMs));
}
