// Estação de solo — receptor 915 MHz do ELE3km.
//
// Escuta o SX1276 onboard (o mesmo rádio do computador de bordo, na mesma placa
// Heltec WiFi LoRa 32 V2) e imprime cada pacote decodificado no Serial a
// 115200 baud. Só recebe: nunca aciona PA, então não há risco de antena solta —
// mas conecte a antena de 915 MHz mesmo assim, senão não ouve nada.
//
// O formato do ar é contrato: ELE3km/PACKET_FORMAT.md e core/telemetry_codec.h.
// Os parâmetros de rádio abaixo copiam hal/radio_sx1276.cpp do firmware; se um
// deles divergir, o receptor não engata no pacote.

#include <Arduino.h>
#include <RadioLib.h>

// ── Pinos do SX1276 onboard da Heltec V2 (iguais ao include/pins.h do firmware) ─
constexpr int PIN_LORA_CS   = 18;
constexpr int PIN_LORA_DIO0 = 26;  // RxDone
constexpr int PIN_LORA_RST  = 14;
constexpr int PIN_LORA_DIO1 = 35;
constexpr int PIN_SPI_SCK   = 5;
constexpr int PIN_SPI_MISO  = 19;
constexpr int PIN_SPI_MOSI  = 27;

// ── Parâmetros de rádio (copiados de hal/radio_sx1276.cpp) ──────────────────────
// 915.0 nominal + ~19 kHz para compensar o desvio de cristal medido entre ESTA
// placa de bordo e ESTE receptor (getFrequencyError ~= +19 kHz, ~21 ppm). Ajuste
// de bancada para ESTE par; o receptor final deve fazer AFC por software.
constexpr float    kFrequencyMhz = 915.019f;  // dentro de 915–928 MHz
constexpr float    kBandwidthKhz = 125.0f;
constexpr uint8_t  kCodingRate   = 8;        // CR 4/8
constexpr uint8_t  kSyncWord     = 0x12;     // default privado do SX127x
constexpr uint16_t kPreamble     = 8;
constexpr int8_t   kOutputPower  = 20;       // irrelevante na recepção; begin() exige

// ⚠️ O transmissor usa SF7 em voo e SF12 no beacon de sobrevivência pós-pouso.
// O receptor ouve UM SF por vez. Deixe 7 para o voo; troque para 12 para caçar o
// beacon depois do pouso.
constexpr uint8_t kSpreadingFactor = 7;

// Contrato do pacote (PACKET_FORMAT.md).
constexpr uint8_t kMagic   = 0xE;
constexpr uint8_t kVersion = 0x1;
constexpr size_t  kFullLen = 20;
constexpr size_t  kAltLen  = 12;

SX1276 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);

volatile bool g_rx_flag = false;
void IRAM_ATTR on_rx_done() { g_rx_flag = true; }

// ── Leitura little-endian do buffer ─────────────────────────────────────────────
// O ESP32 é little-endian, igual ao formato do ar, então memcpy basta.
static uint16_t rd_u16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static int16_t  rd_i16(const uint8_t* p) { int16_t  v; memcpy(&v, p, 2); return v; }
static int32_t  rd_i32(const uint8_t* p) { int32_t  v; memcpy(&v, p, 4); return v; }

static const char* phase_name(uint8_t phase) {
    switch (phase) {
        case 0: return "na-rampa";
        case 1: return "em-voo";
        case 2: return "pousado";
        default: return "?";
    }
}

static const char* source_name(uint8_t src) {
    switch (src) {
        case 0: return "nenhuma";
        case 1: return "GPS";
        case 2: return "inercial";
        case 3: return "ultima-valida";
        default: return "?";
    }
}

// Imprime os campos comuns às duas formas, dados os offsets de flags/saúde/gps e o
// tempo/sequência já lidos. Recebe também altitude e velocidade vertical.
static void print_common(uint16_t seq, uint16_t time_ds, int16_t alt_m,
                         int16_t vspeed_dms, uint8_t flags, uint8_t health,
                         uint8_t gps) {
    const uint8_t phase   = flags & 0x07;
    const uint8_t source  = (flags >> 3) & 0x03;
    const uint8_t bits567 = (flags >> 5) & 0x07;

    Serial.printf("  seq=%u  t=%.1fs  alt=%dm  vz=%.1fm/s\n",
                  seq, time_ds / 10.0f, alt_m, vspeed_dms / 10.0f);
    Serial.printf("  fase=%s  fonte=%s\n", phase_name(phase), source_name(source));

    // Bits 5–7 mudam de significado com a fase (issue 13).
    if (phase == 2) {
        Serial.printf("  amostras-pos-pouso=%u%s\n", bits567,
                      bits567 == 7 ? " (>=7)" : "");
    } else {
        Serial.printf("  qualidade-fix=%u%s\n", bits567,
                      bits567 == 0 ? " (sem fix)" : "");
    }

    // Byte de saúde: bit em 1 = subsistema OK.
    Serial.printf("  saude: IMU=%d baro=%d GPS=%d SD=%d E22=%d SX1276=%d ref-alt=%d\n",
                  (health >> 0) & 1, (health >> 1) & 1, (health >> 2) & 1,
                  (health >> 3) & 1, (health >> 4) & 1, (health >> 5) & 1,
                  (health >> 6) & 1);

    // Byte de GPS: sats nos 4 baixos, HDOP×2 nos 4 altos.
    const uint8_t sats = gps & 0x0F;
    const uint8_t hdop2 = (gps >> 4) & 0x0F;
    Serial.printf("  gps: sats=%u  HDOP=%.1f%s\n", sats, hdop2 * 0.5f,
                  hdop2 == 15 ? " (>=7.5)" : "");
}

static void decode(const uint8_t* buf, size_t len, float rssi, float snr) {
    const uint8_t magic   = (buf[0] >> 4) & 0x0F;
    const uint8_t version = buf[0] & 0x0F;
    if (magic != kMagic || version != kVersion) {
        Serial.printf("[descartado] magic/versao 0x%X/0x%X  (len=%u)\n",
                      magic, version, (unsigned)len);
        return;
    }

    const uint16_t seq     = rd_u16(buf + 1);
    const uint16_t time_ds = rd_u16(buf + 3);

    Serial.printf("[%s]  RSSI=%.1f dBm  SNR=%.1f dB\n",
                  len == kFullLen ? "COMPLETO 20B" : "SO-ALTITUDE 12B", rssi, snr);

    if (len == kFullLen) {
        const int32_t lat = rd_i32(buf + 5);
        const int32_t lon = rd_i32(buf + 9);
        Serial.printf("  lat=%.7f  lon=%.7f\n", lat / 1e7, lon / 1e7);
        print_common(seq, time_ds, rd_i16(buf + 13), rd_i16(buf + 15),
                     buf[17], buf[18], buf[19]);
    } else {  // kAltLen
        Serial.println("  (sem posicao)");
        print_common(seq, time_ds, rd_i16(buf + 5), rd_i16(buf + 7),
                     buf[9], buf[10], buf[11]);
    }
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { /* aguarda USB CDC quando aplicável */ }
    delay(200);

    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_LORA_CS);

    Serial.printf("Receptor ELE3km  |  %.1f MHz  BW %.0f kHz  SF%u  CR4/%u  sync 0x%02X\n",
                  kFrequencyMhz, kBandwidthKhz, kSpreadingFactor, kCodingRate, kSyncWord);

    int state = radio.begin(kFrequencyMhz, kBandwidthKhz, kSpreadingFactor,
                            kCodingRate, kSyncWord, kOutputPower, kPreamble);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("FALHA no radio.begin(): codigo %d — travando.\n", state);
        while (true) { delay(1000); }
    }
    radio.setCRC(true);  // o firmware transmite com CRC de hardware ligado

    radio.setPacketReceivedAction(on_rx_done);
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("FALHA no startReceive(): codigo %d — travando.\n", state);
        while (true) { delay(1000); }
    }
    Serial.println("Escutando...\n");
}

void loop() {
    if (!g_rx_flag) { return; }
    g_rx_flag = false;

    uint8_t buf[32];
    const size_t len = radio.getPacketLength();
    const int state = radio.readData(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
        if (len == kFullLen || len == kAltLen) {
            decode(buf, len, radio.getRSSI(), radio.getSNR());
        } else {
            // PACKET_FORMAT.md: qualquer comprimento que não seja 12 ou 20 é lixo.
            Serial.printf("[descartado] comprimento inesperado: %u B\n", (unsigned)len);
        }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        // RSSI/SNR valem mesmo com CRC quebrado: os registradores são atualizados
        // no RxDone, antes da checagem de CRC. Na bancada, um RSSI muito alto
        // (acima de ~-40 dBm) é o receptor saturado pelo TX ao lado, não sinal bom.
        Serial.printf("[descartado] CRC invalido  RSSI=%.1f dBm  SNR=%.1f dB  len=%u B\n",
                      radio.getRSSI(), radio.getSNR(), (unsigned)len);
        // DIAGNOSTICO DE BANCADA: o buffer ainda foi preenchido mesmo com CRC
        // reprovado. Despeja os bytes crus para distinguir "dado chegou intacto,
        // CRC eh falso-negativo" de "payload realmente corrompido".
        Serial.print("  raw:");
        for (size_t i = 0; i < len; ++i) { Serial.printf(" %02X", buf[i]); }
        const uint8_t dbg_magic = (buf[0] >> 4) & 0x0F;
        const uint8_t dbg_ver   = buf[0] & 0x0F;
        Serial.printf("   (magic=0x%X ver=0x%X %s)\n", dbg_magic, dbg_ver,
                      (dbg_magic == kMagic && dbg_ver == kVersion) ? "OK -> dado intacto, CRC eh o problema"
                                                                   : "RUIM -> payload corrompido");
        // Erro de frequencia TX<->RX medido pelo SX1276 neste pacote. Dezenas de
        // kHz = desvio de cristal entre as placas (corrompe payload, deprime SNR,
        // independe da potencia). Perto de 0 = descartar essa hipotese.
        Serial.printf("  ferr=%.0f Hz\n", radio.getFrequencyError());
    } else {
        Serial.printf("[erro] readData(): codigo %d\n", state);
    }

    radio.startReceive();  // volta a escutar
}
