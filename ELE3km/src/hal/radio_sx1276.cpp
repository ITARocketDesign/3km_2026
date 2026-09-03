#include "hal/radio_sx1276.h"

#include <Arduino.h>
#include <RadioLib.h>

#include "pins.h"

namespace hal {
namespace {

// Parâmetros de rádio. Comuns aos dois caminhos: BW 125 kHz, CR 4/8, CRC ligado,
// cabeçalho explícito, preâmbulo de 8 símbolos.
constexpr float   kFrequencyMhz = 915.0f;  // dentro de 915–928 MHz
constexpr float   kBandwidthKhz = 125.0f;
constexpr uint8_t kFlightSpreadingFactor = 7;
constexpr uint8_t kSurvivalSpreadingFactor = 12;
constexpr uint8_t kCodingRateDenominator = 8;  // CR 4/8
constexpr uint8_t kSyncWord = 0x12;            // default privado do SX127x
constexpr int8_t  kOutputPowerDbm = 2;         // BANCADA: reduzido de +20 dBm (PRD §5) p/ não saturar o RX ao lado — reverter p/ 20 antes de teste de alcance
constexpr uint16_t kPreambleSymbols = 8;

// Margem sobre o airtime calculado em runtime antes de declarar a transmissão
// perdida. Vale tanto para SF7 / 1 s quanto para SF12 / 20 s.
constexpr uint32_t kTxTimeoutMultiplier = 3;
constexpr uint32_t kTxTimeoutFloorMs = 100;

// Há exatamente um SX1276 na placa, e RadioLib entrega a interrupção por
// ponteiro de função — então o objeto e a bandeira são de escopo de arquivo.
// Esta é a HAL; a regra de "nenhuma variável global" vale para o core/.
SX1276 g_radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
volatile bool g_tx_done_edge = false;

void IRAM_ATTR on_tx_done() {
    // A borda sozinha não prova nada (§12): quem decide é service(), lendo o
    // registrador de status.
    g_tx_done_edge = true;
}

}  // namespace

bool RadioSx1276::begin(RadioSx1276Mode mode) {
    mode_ = mode;
    initialized_ = false;
    tx_in_progress_ = false;
    g_tx_done_edge = false;

    const uint8_t spreading_factor =
        mode_ == RadioSx1276Mode::SurvivalBeacon ? kSurvivalSpreadingFactor
                                                 : kFlightSpreadingFactor;
    const int16_t state =
        g_radio.begin(kFrequencyMhz, kBandwidthKhz, spreading_factor,
                      kCodingRateDenominator, kSyncWord, kOutputPowerDbm,
                      kPreambleSymbols);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    if (g_radio.setCRC(true) != RADIOLIB_ERR_NONE) {
        return false;
    }

    g_radio.setPacketSentAction(on_tx_done);
    initialized_ = true;
    return true;
}

bool RadioSx1276::start_send(const uint8_t* data, uint8_t len, uint32_t now_ms) {
    if (!is_ready() || data == nullptr || len == 0) {
        return false;
    }

    g_tx_done_edge = false;
    if (g_radio.startTransmit(data, len) != RADIOLIB_ERR_NONE) {
        return false;
    }

    const uint32_t airtime_ms = static_cast<uint32_t>(g_radio.getTimeOnAir(len) / 1000);
    uint32_t budget_ms = airtime_ms * kTxTimeoutMultiplier;
    if (budget_ms < kTxTimeoutFloorMs) {
        budget_ms = kTxTimeoutFloorMs;
    }

    tx_in_progress_ = true;
    tx_deadline_ms_ = now_ms + budget_ms;
    return true;
}

void RadioSx1276::service(uint32_t now_ms) {
    if (!tx_in_progress_) {
        return;
    }

    if (g_tx_done_edge) {
        g_tx_done_edge = false;
        // Confirmação obrigatória contra o registrador de status. Uma borda sem
        // a bandeira correspondente é fantasma e é ignorada — a transmissão
        // segue em andamento até o registrador concordar ou o timeout estourar.
        if ((g_radio.getIrqFlags() & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE) != 0) {
            finish_tx();
            return;
        }
    }

    // Nunca esperar indefinidamente: passado o prazo, o rádio é resetado e
    // reinicializado. Um rádio mudo que volta é melhor que uma task travada.
    if (static_cast<int32_t>(now_ms - tx_deadline_ms_) >= 0) {
        ++timeout_count_;
        recover();
    }
}

void RadioSx1276::finish_tx() {
    g_radio.finishTransmit();  // limpa as bandeiras e volta para standby
    tx_in_progress_ = false;
}

void RadioSx1276::recover() {
    tx_in_progress_ = false;
    g_radio.reset();
    // Se a reinicialização falhar, initialized_ fica false e o rádio para de
    // aceitar transmissões até uma nova tentativa. A política de retentativa
    // periódica é da issue 10.
    begin(mode_);
}

}  // namespace hal
