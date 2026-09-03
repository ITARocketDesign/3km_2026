// ELE3km — computador de bordo.
//
// Issue 05: a placa deixa de ser um laço único e passa a rodar DUAS tasks
// FreeRTOS fixadas em cores diferentes, com um ring buffer SPSC lock-free entre
// elas. É a separação de concorrência que impede o cartão de engolir amostras de
// sensor: um microSD trava por 100–250 ms durante coleta de lixo interna, e num
// superloop isso vira lacuna nas amostras — viés no passo de predição do filtro
// durante o boost, não ruído.
//
//   • `flight` (core 1, prioridade alta, 8 KB) — adquire barômetro e GPS, roda o
//     estimador e as decisões, e ENFILEIRA: o registro de log no ring, os comandos
//     de transmissão no tx-ring. Toca só I²C e UART. Nunca espera o cartão.
//   • `io` (core 0, prioridade média, 12 KB) — dono exclusivo do SPI: drena o ring
//     para o cartão, executa as transmissões que o escalonador decidiu, e devolve
//     `write_in_progress` para o núcleo. Uma travada de cartão fica presa aqui, no
//     core 0, e o core 1 não percebe.
//
// A regra de exclusão que sobra mora no núcleo (core/tx_scheduler.h): escrita em
// andamento barra transmissão (disputa do barramento SPI). A task io levanta
// `write_in_progress` em volta da escrita real, e o escalonador na task flight o lê.
//
// Um único rádio: o SX1276 onboard do Heltec. O E22 de 433 MHz foi removido do
// projeto (Docs/ELE3km_drop_e22_single_radio.md). O modo de sobrevivência da issue
// 12 continua, agora só trocando o SX1276 para SF12/20 s. Uma falha de log nunca
// cala o link de recuperacao.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

#include "core/flight_computer.h"
#include "core/ring_buffer.h"
#include "core/stack_health.h"
#include "core/telemetry_codec.h"
#include "hal/board.h"
#include "hal/bmp280.h"
#include "hal/boot_loop.h"
#include "hal/boot_counter.h"
#include "hal/i2c_bus.h"
#include "hal/mpu6050.h"
#include "hal/flight_state.h"
#include "hal/gps_neo6m.h"
#include "hal/radio_sx1276.h"
#include "hal/sd_log.h"
#include "pins.h"

// ── Seletor de fase de BANCADA — escolha de compilação ──────────────────────────
// Troque BENCH_FORCE_PHASE abaixo para forçar uma fase JÁ NO BOOT, sem depender do
// Serial. AUTO é a versão de VOO: a máquina de fases real decide. Qualquer outro
// valor é um build DE BANCADA — a placa sobe presa naquela fase.
//
// A trava contra subir um build forçado por engano: um valor não-AUTO dispara um
// #warning no log de compilação E imprime um banner gritado no boot. A versão que
// vai ao foguete TEM que ser AUTO, e o build avisa alto se não for.
//
// (Também aceita -D BENCH_FORCE_PHASE=BENCH_PHASE_FLIGHT via build_flags, se um dia
//  você quiser um env de bancada dedicado; o default aqui continua AUTO.)
#define BENCH_PHASE_AUTO   0
#define BENCH_PHASE_RAIL   1
#define BENCH_PHASE_FLIGHT 2
#define BENCH_PHASE_LANDED 3

#ifndef BENCH_FORCE_PHASE
#define BENCH_FORCE_PHASE BENCH_PHASE_FLIGHT   // <<<<< troque aqui para testar uma fase
#endif

#if BENCH_FORCE_PHASE != BENCH_PHASE_AUTO
#warning "BENCH_FORCE_PHASE != AUTO: build DE BANCADA com fase forcada — NAO e versao de voo."
#endif

namespace {

// Traduz a escolha de compilação para o enum do núcleo. É o valor inicial do
// seletor; o Serial ainda pode trocá-lo em runtime por cima disto.
constexpr core::PhaseOverride kBootPhaseOverride =
#if   BENCH_FORCE_PHASE == BENCH_PHASE_RAIL
    core::PhaseOverride::Rail;
#elif BENCH_FORCE_PHASE == BENCH_PHASE_FLIGHT
    core::PhaseOverride::Flight;
#elif BENCH_FORCE_PHASE == BENCH_PHASE_LANDED
    core::PhaseOverride::Landed;
#else
    core::PhaseOverride::Auto;
#endif

// 100 Hz (Δt = 10 ms): a cadência de projeto do ciclo (PRD §3, 64 B × 100 Hz =
// 6,4 kB/s). O barômetro é lido a 25 Hz dentro dele; o canal inercial a 100 Hz
// entra na issue 07 sem mudar esta cadência.
constexpr uint32_t kCyclePeriodMs = 10;
constexpr uint32_t kBaroPeriodMs = 40;  // 25 Hz

// Despejo de diagnóstico dos sensores no Serial durante teste de bancada (sem
// cartão). A 100 Hz encheria o buffer da UART e travaria o ciclo de voo; a alguns
// hertz é inofensiva. Ponha 0 para desligar.
constexpr uint32_t kSensorPrintPeriodMs = 250;

// ── Estado tocado por uma única task ────────────────────────────────────────
// Sensores: só a task flight (I²C/UART). Rádio e cartão: só a task io (SPI).
hal::Bmp280          g_baro(Wire);
hal::Mpu6050         g_imu(Wire);
hal::GpsNeo6m        g_gps(Serial1);
hal::RadioSx1276     g_sx1276;
hal::SdLog           g_log;
hal::BootCounter     g_boot_counter;
hal::FlightState     g_flight_state;
core::FlightComputer g_computer;

// ── Estado compartilhado entre as duas tasks ────────────────────────────────
// O ring do log (drop-oldest) e o tx-ring (comandos de rádio), os dois SPSC:
// flight produz, io consome.
core::RingBuffer g_log_ring;
core::TxRing     g_tx_ring;

// O sinal de arbitragem que atravessa entre os cores: io levanta write_in_progress
// em volta da escrita real e a task flight o lê para barrar transmissão enquanto o
// barramento SPI está ocupado. Um único escritor (io), como os índices do ring.
std::atomic<bool> g_write_in_progress{false};  // io → flight (disputa de barramento)

// Persistência da fase (issue 06), flight → io. A gravação em NVS bloqueia alguns
// milissegundos e não pode rodar na task de voo a 100 Hz; a task flight só monta o
// snapshot e ergue a flag, e a task io grava. São ~5 eventos por voo (liftoff e
// transições de fase), então a flag quase nunca está levantada.
core::PhaseSnapshot g_pending_snapshot;
std::atomic<bool>   g_snapshot_dirty{false};   // flight publica, io consome

// Watermark de stack da task io (issue 11), io → flight. O registro é montado e
// codificado na task flight, mas a folga de stack da io só a io conhece: ela
// amostra a própria a cada ~1 s e publica aqui, e a flight lê e carimba no
// registro ao lado da sua. Um escritor, um leitor — o mesmo padrão SPSC dos
// índices do ring. É diagnóstico só-de-log; não atravessa para o pacote de rádio.
std::atomic<uint16_t> g_io_watermark{0};

// Saúde dos subsistemas que a task io possui — o cartão e o rádio — publicada para
// a task flight compor o bitmap de saúde do byte 18 (issue 10). Um escritor (io), um
// leitor (flight): o mesmo padrão SPSC do watermark. Vai empacotada já nos bits de
// health_bit; imu, baro e gps entram pela amostra, não por aqui.
std::atomic<uint8_t> g_io_health{0};  // io → flight

// Seletor de fase de BANCADA (guarda o valor de core::PhaseOverride). Começa no
// valor de compilação (kBootPhaseOverride); a task io lê as teclas do Serial e o
// troca em runtime; a task flight lê e passa ao update(). Auto é o comportamento de
// voo — a máquina de fases real decide. Escritor: io; leitor: flight.
std::atomic<uint8_t> g_phase_override{
    static_cast<uint8_t>(kBootPhaseOverride)};  // io → flight

// A TWDT do framework (arduino-esp32 2.0.17 / IDF 4.4) roda com um ÚNICO timeout
// global de 5 s — a API desta versão não tem timeout por task. As duas tasks se
// inscrevem e alimentam a cada volta; um deadlock real numa delas é pego em ~5 s e
// vira reset limpo, que reentra no caminho de recuperação (boot counter novo,
// arquivo novo, varredura recupera o anterior). Os 3 s da task flight pedidos na
// issue exigiriam timeout por task (IDF 5), fora do framework fixado — ver o
// Estado da implementação da issue 11.
constexpr uint32_t kWatermarkPeriodMs = 1000;  // amostragem da folga de stack (§14)

// uxTaskGetStackHighWaterMark() devolve UBaseType_t (32 bits) em bytes; o campo do
// registro é u16. Os stacks são de 8 KB e 12 KB, então o valor cabe; o clamp é só
// rede de segurança.
uint16_t clamp_watermark(UBaseType_t words_free) {
    return words_free > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu)
                                : static_cast<uint16_t>(words_free);
}

// Resultado do boot. Escrito no setup(), antes de as tasks subirem; depois cada
// flag é lida só pela task dona do seu barramento.
bool     g_baro_ok = false;   // flight
bool     g_imu_ok = false;    // flight
bool     g_gps_ok = false;    // flight
bool     g_sx1276_ok = false; // io
bool     g_log_ok = false;    // io
uint16_t g_boot_count = 0;
core::BootLoopStatus g_boot_loop_status;

// Despeja a amostra de sensores crua (mais a altitude derivada) no Serial. Só para
// bancada: mostra o que baro, IMU e GPS estão entregando sem depender do cartão. O
// '--' marca leitura não confiável nesta amostra; 'SAT' marca acelerômetro no fundo
// de escala.
void print_sensor_sample(const core::SensorSample& s, const core::LogRecord& log) {
    Serial.printf("baro: %s P=%.1f Pa  T=%.2f C  alt=%.1f m\n",
                  s.baro_valid ? "ok" : "--", s.pressure_pa, s.temperature_c,
                  log.altitude_m);
    Serial.printf("imu:  %s%s a=[%d %d %d] mg  g=[%d %d %d] ddps\n",
                  s.imu_valid ? "ok" : "--", s.accel_saturated ? " SAT" : "",
                  s.accel_mg[0], s.accel_mg[1], s.accel_mg[2],
                  s.gyro_ddps[0], s.gyro_ddps[1], s.gyro_ddps[2]);
    Serial.printf("gps:  recv=%d fix=%d q=%u sats=%u HDOP=%.1f lat=%.7f lon=%.7f\n\n",
                  s.gps.receiving, s.gps.valid, s.gps.fix_quality, s.gps.satellites,
                  s.gps.hdop_half * 0.5f, s.gps.latitude_1e7 / 1e7,
                  s.gps.longitude_1e7 / 1e7);
}

// A task flight: adquire, decide, enfileira. Nunca toca SPI, nunca espera o cartão.
void flight_task(void*) {
    uint32_t next_baro_ms = millis();
    TickType_t last_wake = xTaskGetTickCount();

    // Inscreve esta task na TWDT: a partir daqui ela precisa alimentar o watchdog a
    // cada volta ou o timeout global dispara o reset.
    esp_task_wdt_add(nullptr);

    // Folga de stack da própria task, amostrada a cada ~1 s e carimbada em cada
    // registro. Começa "não amostrada"; o primeiro carimbo real vem em até 1 s.
    uint32_t next_wm_ms = millis();
    uint16_t flight_wm = 0;
    bool     wm_warned = false;

    // Throttle do despejo de sensores na bancada (independente do watchdog e do log).
    uint32_t next_print_ms = millis();

    for (;;) {
        const uint32_t now_ms = millis();

        if (static_cast<int32_t>(now_ms - next_wm_ms) >= 0) {
            next_wm_ms = now_ms + kWatermarkPeriodMs;
            flight_wm  = clamp_watermark(uxTaskGetStackHighWaterMark(nullptr));
            if (!wm_warned && core::stack_watermark_degraded(flight_wm)) {
                Serial.printf("AVISO: stack da task flight em %u B (DEGRADED)\n", flight_wm);
                wm_warned = true;
            }
        }

        // A UART do GPS é drenada a cada volta: o buffer tem ~530 ms de fôlego e
        // não há razão para gastar essa margem.
        if (g_gps_ok) {
            g_gps.service(now_ms);
        }

        core::SensorSample sample;  // defaults declaram "sem leitura fresca"

        // Recuperação de barramento I²C (issue 09). Uma leitura que falha depois do
        // timeout duro do adaptador é um barramento travado (H5): a rotina solta a
        // linha, clocka o escravo preso para fora e reinicializa o driver, e o
        // begin() reaplica timeout e configuração. NO MÁXIMO UM módulo por ciclo — a
        // recuperação custa ~10–50 ms e não pode comer o ciclo de forma sustentada;
        // se o begin() falhar, o módulo fica ausente e para de ser lido (nada mais o
        // reergue nesta fatia — a retentativa periódica de 5 s é da issue 10).
        bool i2c_recovered = false;

        // A IMU é lida a cada ciclo (100 Hz): é o canal inercial que o estimador
        // integra no passo de predição e a fonte do |a| do liftoff. O adaptador
        // marca saturação de fundo de escala, que o estimador exclui da predição.
        if (g_imu_ok) {
            sample.imu_valid =
                g_imu.read(sample.accel_mg, sample.gyro_ddps, sample.accel_saturated);
            if (!sample.imu_valid) {
                // Timestamp na aquisição (AC7): correlacionável com os timestamps de
                // TX do escalonador na análise pós-voo. O carimbo durável no cartão
                // (bitmap de saúde, contador de reinit) é da issue 10.
                Serial.printf("I2C: erro na IMU em t=%lu ms; recuperando barramento\n",
                              static_cast<unsigned long>(now_ms));
                hal::i2c_bus_recover();
                g_imu_ok      = g_imu.begin();
                i2c_recovered = true;
            }
        }

        if (g_baro_ok && static_cast<int32_t>(now_ms - next_baro_ms) >= 0) {
            next_baro_ms      = now_ms + kBaroPeriodMs;
            sample.baro_valid = g_baro.read(sample.pressure_pa, sample.temperature_c);
            if (!sample.baro_valid && !i2c_recovered) {
                Serial.printf("I2C: erro no baro em t=%lu ms; recuperando barramento\n",
                              static_cast<unsigned long>(now_ms));
                hal::i2c_bus_recover();
                g_baro_ok     = g_baro.begin();
                i2c_recovered = true;
            }
        }
        if (g_gps_ok) {
            sample.gps                = g_gps.fix(now_ms);
            sample.gps_uart_overflows = g_gps.uart_overflow_count();
        }

        const bool wip = g_write_in_progress.load(std::memory_order_acquire);

        // Saúde do cartão e do rádio, publicada pela task io, para o bitmap de
        // saúde do byte 18 (issue 10). imu/baro/gps já entram pela amostra.
        const uint8_t io_bits = g_io_health.load(std::memory_order_acquire);
        core::IoSubsystemHealth io_health;
        io_health.sd     = (io_bits & core::health_bit::kSd) != 0;
        io_health.sx1276 = (io_bits & core::health_bit::kSx1276) != 0;

        // Seletor de bancada: em Auto (voo) a máquina de fases decide; qualquer
        // outro valor pina a fase para conferir os sensores em cada fase (real).
        const auto phase_override = static_cast<core::PhaseOverride>(
            g_phase_override.load(std::memory_order_acquire));

        core::UpdateResult result =
            g_computer.update(sample, now_ms, wip, io_health, phase_override);

        // Diagnóstico de folga de stack no registro (issue 11): a da própria task e
        // a da io publicada pela io. Só no cartão — não cabe nos 20 B do rádio.
        result.log.stack_watermark_flight = flight_wm;
        result.log.stack_watermark_io     = g_io_watermark.load(std::memory_order_acquire);

        // Despejo de sensores na bancada, a alguns hertz. Depois do update() para a
        // altitude derivada (result.log.altitude_m) já estar preenchida.
        if (kSensorPrintPeriodMs > 0 &&
            static_cast<int32_t>(now_ms - next_print_ms) >= 0) {
            next_print_ms = now_ms + kSensorPrintPeriodMs;
            print_sensor_sample(sample, result.log);
        }

        // O registro vai codificado para o ring; a task io grava esses bytes no
        // cartão sem tocar no formato. Enfileirar nunca bloqueia.
        uint8_t record[core::kLogRecordSize];
        core::encode_record(result.log, g_boot_count, record, sizeof(record));
        g_log_ring.push(record);

        // Liftoff ou transição de fase: monta o snapshot e passa para a task io
        // gravar em NVS. Nunca grava aqui — o flash bloquearia o ciclo de 100 Hz.
        // Se a flag ainda estiver levantada (io não gravou o anterior), o evento
        // atual espera o próximo ciclo; transições distam segundos, io grava em ms.
        if (result.persist_phase && !g_snapshot_dirty.load(std::memory_order_acquire)) {
            g_pending_snapshot = g_computer.phase_snapshot();
            g_snapshot_dirty.store(true, std::memory_order_release);
        }

        // As transmissões que o escalonador liberou viram comandos para a task io.
        for (uint8_t i = 0; i < result.packet_count; ++i) {
            const core::TelemetryPacket& packet = result.packets[i];

            uint8_t payload[core::kMaxPacketSize];
            const size_t len = core::encode_packet(packet, payload, sizeof(payload));
            if (len == 0) {
                continue;
            }
            uint8_t slot[core::TxRing::kSlotSize] = {0};
            slot[0] = static_cast<uint8_t>(len);
            memcpy(slot + 1, payload, len);
            g_tx_ring.push(slot);

            // O detector de brown-out do receptor GPS precisa saber quando um PA
            // disparou: satélites indo a zero logo depois é rail afundando, não céu
            // ruim. O escalonador acabou de liberar a transmissão, então o PA vai
            // disparar já — e o GPS é da task flight, então quem anota é ela.
            if (g_gps_ok) {
                g_gps.note_transmission(now_ms);
            }
        }

        // Alimenta o watchdog uma vez por iteração. A 100 Hz, folga de sobra contra
        // o timeout de 5 s: só um deadlock real deixa de chamar isto.
        esp_task_wdt_reset();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCyclePeriodMs));
    }
}

// Nome legível de um modo do seletor.
const char* override_name(core::PhaseOverride o) {
    switch (o) {
        case core::PhaseOverride::Rail:   return "RAMPA (forcada)";
        case core::PhaseOverride::Flight: return "VOO (forcado)";
        case core::PhaseOverride::Landed: return "POUSADO (forcado)";
        default:                          return "AUTO (maquina real)";
    }
}

// Ajuda do seletor de bancada, impressa no boot e ao digitar '?'.
void print_selector_help() {
    Serial.println("── seletor de fase (bancada) ─────────────────────────────");
    Serial.println("  g = forca RAMPA    f = forca VOO    l = forca POUSADO");
    Serial.println("  a = AUTO (maquina real decide)      ? = esta ajuda");
    Serial.println("  a fase e forcada; barometro/IMU/GPS seguem reais.");
    Serial.printf ("  modo de compilacao (BENCH_FORCE_PHASE): %s\n",
                   override_name(kBootPhaseOverride));
    Serial.println("──────────────────────────────────────────────────────────");
}

// Banner gritado no boot quando o BUILD já sobe com uma fase forçada. É a trava
// contra subir ao foguete uma versão que não seja AUTO: impossível de não ver.
void warn_if_forced_build() {
    if (kBootPhaseOverride == core::PhaseOverride::Auto) return;
    Serial.println();
    Serial.println("##########################################################");
    Serial.println("#  ATENCAO: BUILD DE BANCADA — FASE FORCADA NA COMPILACAO #");
    Serial.printf ("#  fase: %-46s#\n", override_name(kBootPhaseOverride));
    Serial.println("#  ESTA NAO E A VERSAO DE VOO. Compile com AUTO p/ voar.  #");
    Serial.println("##########################################################");
    Serial.println();
}

// Lê as teclas do seletor no Serial e publica o modo para a task flight. Só troca
// qual fase o núcleo enxerga; nunca falseia sensor. Roda na task io (dona do Serial
// de status). Em voo, sem USB, nada chega e o modo fica em Auto.
void poll_phase_selector() {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        core::PhaseOverride next;
        const char* name;
        switch (c) {
            case 'g': case 'G': next = core::PhaseOverride::Rail;   name = "RAMPA (forcada)";   break;
            case 'f': case 'F': next = core::PhaseOverride::Flight; name = "VOO (forcado)";      break;
            case 'l': case 'L': next = core::PhaseOverride::Landed; name = "POUSADO (forcado)";  break;
            case 'a': case 'A': next = core::PhaseOverride::Auto;   name = "AUTO (maquina real)"; break;
            case '?':                 print_selector_help();      continue;
            case '\r': case '\n': case ' ': continue;  // ignora terminadores de linha
            default:
                Serial.printf("seletor: tecla '%c' desconhecida — g/f/l/a, ? ajuda\n",
                              static_cast<char>(c));
                continue;
        }
        g_phase_override.store(static_cast<uint8_t>(next), std::memory_order_release);
        Serial.printf(">> fase forcada: %s\n", name);
    }
}

// A task io: dona do SPI. Serve os rádios, executa as transmissões decididas, e
// drena o ring para o cartão. É aqui que a travada do cartão fica presa.
void io_task(void*) {
    // Inscreve a task io na TWDT. A pior stall aqui é uma escrita de SD (~500 ms) —
    // bem abaixo do timeout de 5 s; só um travamento real deixa de alimentar.
    esp_task_wdt_add(nullptr);

    uint32_t next_wm_ms = millis();
    bool     wm_warned = false;

    for (;;) {
        const uint32_t now_ms = millis();

        // Seletor de bancada: lê as teclas do Serial e publica o modo de fase.
        poll_phase_selector();

        if (static_cast<int32_t>(now_ms - next_wm_ms) >= 0) {
            next_wm_ms = now_ms + kWatermarkPeriodMs;
            const uint16_t io_wm = clamp_watermark(uxTaskGetStackHighWaterMark(nullptr));
            g_io_watermark.store(io_wm, std::memory_order_release);
            if (!wm_warned && core::stack_watermark_degraded(io_wm)) {
                Serial.printf("AVISO: stack da task io em %u B (DEGRADED)\n", io_wm);
                wm_warned = true;
            }
        }

        // Confirma o fim da transmissão anterior contra o registrador de status e
        // aplica o timeout. Precisa rodar a cada volta.
        g_sx1276.service(now_ms);

        // Executa os comandos de transmissão que a task flight enfileirou. A task
        // io não tem política própria: só põe no ar o que o escalonador decidiu.
        uint8_t slot[core::TxRing::kSlotSize];
        while (g_tx_ring.pop(slot)) {
            const uint8_t  len = slot[0];
            const uint8_t* payload = slot + 1;

            const bool sent = g_sx1276_ok && g_sx1276.start_send(payload, len, now_ms);
            (void)sent;
        }

        // Drena o ring para o bloco em montagem, sem passar de um bloco cheio: o
        // que não couber fica no ring, que é o buffer elástico. O descarte do mais
        // antigo, se o cartão travar demais, acontece lá, não aqui.
        if (g_log_ok) {
            uint8_t record[core::kLogRecordSize];
            while (!g_log.block_ready() && g_log_ring.pop(record)) {
                g_log.stage(record);
            }

            // Enquanto a escrita roda, write_in_progress barra qualquer transmissão
            // (disputa do barramento SPI) — e ela nunca é interrompida. Não há mais
            // disputa de rail: o SX1276 vive no regulador de 5 V do Heltec.
            if (g_log.block_ready()) {
                g_write_in_progress.store(true, std::memory_order_release);
                g_log.service(now_ms);
                g_write_in_progress.store(false, std::memory_order_release);

                g_log_ok = g_log.is_open();
            }
        }

        // Persistência da fase em NVS, quando a task flight pediu. A gravação
        // bloqueia alguns milissegundos — segura aqui no core 0, longe do ciclo de
        // voo. Uma falha de NVS não cala o link: a FlightState apenas não grava.
        if (g_snapshot_dirty.load(std::memory_order_acquire)) {
            g_flight_state.save(g_pending_snapshot);
            g_snapshot_dirty.store(false, std::memory_order_release);
        }

        // Publica a saúde dos subsistemas da io (cartão e rádio) para a task flight
        // compor o bitmap de saúde do byte 18 (issue 10). O bit do cartão acompanha
        // g_log_ok, que a lógica de travadas acima pode ter baixado.
        const uint8_t io_bits =
            static_cast<uint8_t>((g_log_ok ? core::health_bit::kSd : 0) |
                                 (g_sx1276_ok ? core::health_bit::kSx1276 : 0));
        g_io_health.store(io_bits, std::memory_order_release);

        // Alimenta o watchdog uma vez por iteração, depois da escrita de SD, que é a
        // operação mais longa desta task.
        esp_task_wdt_reset();

        // Uma volta curta: responsivo aos rádios e ao ring sem monopolizar o core.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

}  // namespace

void setup() {
    // Antes de qualquer outra coisa: chip-selects em nível seguro, rádio sem dono
    // desselecionado, WiFi e Bluetooth desligados.
    hal::board_early_init();

    // Issue 12: a decisao acontece ANTES de o rádio subir. O historico usa o relogio
    // e a memoria retida do RTC; o contador do arquivo continua em NVS. Depois de
    // latcheado, so power-on limpo apaga o modo.
    g_boot_loop_status = hal::detect_boot_loop_at_boot();
    g_boot_count       = g_boot_counter.next();

    core::FlightComputerConfig computer_config;
    if (g_boot_loop_status.active) {
        computer_config.tx.mode = core::TxMode::SurvivalBeacon;
    }
    g_computer = core::FlightComputer(computer_config);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ELE3km — issue 12: boot loop e beacon de sobrevivencia");
    if (g_boot_loop_status.active) {
        Serial.printf("BOOT LOOP: %u resets recentes; beacon de sobrevivencia (SF12)\n",
                      static_cast<unsigned>(g_boot_loop_status.recent_reset_count));
    }

    // Só agora o barramento SPI pode subir.
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

    const hal::RadioSx1276Mode sx1276_mode =
        g_boot_loop_status.active ? hal::RadioSx1276Mode::SurvivalBeacon
                                  : hal::RadioSx1276Mode::Flight;
    // ⚠️ Nunca ligar a placa sem a antena do SX1276 conectada: o firmware não
    // detecta antena solta, e transmitir em circuito aberto danifica o PA.
    g_sx1276_ok = g_sx1276.begin(sx1276_mode);
    Serial.printf("SX1276: %s\n",
                  g_sx1276_ok
                      ? (g_boot_loop_status.active ? "ok, 915 MHz, SF12, +20 dBm"
                                                   : "ok, 915 MHz, SF7, +20 dBm")
                      : "FALHOU");

    // Restaura fase e referência de um eventual reset (issue 06). A FlightState lê
    // o snapshot salvo e o motivo do reset; a decisão de reusar é do núcleo. Sem
    // reuso, a máquina começa na rampa e constrói a média lenta.
    const core::PhaseRestore restore = g_flight_state.restore();
    g_computer.begin(restore);

    // A referência de solo do cabeçalho do arquivo: a salva, quando o reset a fez
    // ser reusada; senão a primeira leitura válida do barômetro. A média lenta na
    // rampa e o congelamento no liftoff correm na máquina de fases durante o voo.
    const core::PhaseSnapshot restored = g_computer.phase_snapshot();
    float reference_pa = 0.0f;
    if (restored.has_reference) {
        reference_pa = restored.reference_pa;
    } else if (g_baro_ok) {
        float temperature_c = 0.0f;
        g_baro.read(reference_pa, temperature_c);
    }
    Serial.printf("fase: %s%s\n",
                  restore.have_saved ? "snapshot lido" : "sem snapshot",
                  (restored.phase == core::FlightPhase::Flight) ? ", referencia reusada" : "");

    g_log_ok = g_log.begin(g_boot_count, reference_pa, g_boot_loop_status.active,
                           g_boot_loop_status.recent_reset_count);
    Serial.printf("microSD: %s (voo %u)\n",
                  g_log_ok ? "arquivo pré-alocado, cabeçalho gravado" : "AUSENTE",
                  static_cast<unsigned>(g_boot_count));

    print_selector_help();
    warn_if_forced_build();

    // As duas tasks sobem fixadas em cores diferentes. Stacks conservadores por
    // escolha (issue 05): io roda SdFat e RadioLib; flight roda os três canais
    // Kalman do estimador (07). Depurar stack overflow numa placa que já voou é
    // caro; sobra SRAM.
    xTaskCreatePinnedToCore(flight_task, "flight", 8192, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(io_task, "io", 12288, nullptr, 2, nullptr, 0);
}

// Todo o trabalho está nas duas tasks. O loopTask do Arduino não faz nada além de
// ceder o core.
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
