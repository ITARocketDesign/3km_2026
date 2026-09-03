// include/pins.h — mapa de pinos do ELE3km. FONTE ÚNICA.
//
// Revisão da placa .......... rev. 1
// Origem .................... netlist KiCad exportado em 2026-07-25
//                             (Docs/ELE3km_connections.md, que vence sobre
//                              E22_integration.md em qualquer divergência)
// Delta aplicado ............ PRD v3 §0 (R2, R3) e §13 — o SX1276 onboard
//                             passa a transmitir, então GPIO18 é um
//                             chip-select normal e GPIO14 é o reset dele,
//                             controlado pela biblioteca de rádio.
//
// Nenhum pino pode ser escrito em número literal em outro arquivo. Se a placa
// for revisada, este arquivo muda e mais nada.
//
// Antes de usar qualquer coisa daqui, leia DISCIPLINE.md: metade destes pinos
// tem uma forma de uso que destrói hardware.
#pragma once

#include <stdint.h>

// Gravado no cabeçalho do log (PRD §8) para que uma análise pós-voo saiba
// contra qual fiação o firmware rodou.
constexpr uint8_t PIN_MAP_REVISION = 1;

// ── VSPI compartilhada ──────────────────────────────────────────────────────
// Três dispositivos no mesmo barramento: SX1276 onboard, microSD e E22.
constexpr int PIN_SPI_SCK  = 5;
constexpr int PIN_SPI_MISO = 19;
constexpr int PIN_SPI_MOSI = 27;

// ── Chip-selects: exatamente um ativo por vez (PRD §13) ─────────────────────
constexpr int PIN_SD_CS   = 23;  // sem pull-up; flutua no reset
constexpr int PIN_E22_NSS = 32;  // sem pull-up; flutua no reset
constexpr int PIN_LORA_CS = 18;  // sem pull-up; flutua no reset (hazard H2)

// ── SX1276 onboard (915–928 MHz) — interno ao módulo Heltec ─────────────────
constexpr int PIN_LORA_RST  = 14;
constexpr int PIN_LORA_DIO0 = 26;  // TxDone / RxDone
constexpr int PIN_LORA_DIO1 = 35;  // input-only
constexpr int PIN_LORA_DIO2 = 34;  // input-only, não usado

// ── E22-400M30S / SX1268 (433 MHz, 1 W) — entra na issue 03 ─────────────────
constexpr int PIN_E22_NRST = 33;  // sem pull-up externo (R3 não montado)
constexpr int PIN_E22_BUSY = 36;  // INPUT ONLY, errata de glitch (H6)
constexpr int PIN_E22_DIO1 = 39;  // INPUT ONLY, errata de glitch (H6)
constexpr int PIN_E22_TXEN = 25;  // só a biblioteca de rádio escreve aqui (H8)
constexpr int PIN_E22_RXEN = 12;  // idem, e é strapping pin (H7)

// ── I²C: MPU6050 + BMP280 ───────────────────────────────────────────────────
constexpr int PIN_I2C_SDA = 21;  // também controla o Vext do módulo (H9)
constexpr int PIN_I2C_SCL = 22;

// ── UART do GPS (NEO-6M) — entra na issue 02 ────────────────────────────────
constexpr int PIN_GPS_TX = 13;  // ESP32 transmite → pad RX do GPS
constexpr int PIN_GPS_RX = 17;  // ESP32 recebe   ← pad TX do GPS

// ── Endereços e taxas de barramento ─────────────────────────────────────────
constexpr uint8_t  ADDR_MPU6050        = 0x68;  // sondar 0x69 também
constexpr uint8_t  ADDR_MPU6050_ALT    = 0x69;  // AD0 em nível alto
constexpr uint8_t  ADDR_BMP280         = 0x76;  // sondar 0x77 também
constexpr uint8_t  ADDR_BMP280_ALT     = 0x77;
constexpr uint32_t I2C_HZ              = 100000;  // não subir para 400k (H9)
constexpr uint32_t GPS_BAUD            = 9600;
