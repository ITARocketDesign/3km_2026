#include "core/tx_scheduler.h"

namespace core {
namespace {

// Comparação de instantes que sobrevive ao wrap de uint32_t (~49 dias). Vale
// para todo o núcleo: o tempo entra como parâmetro e nunca é comparado direto.
bool reached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

constexpr uint32_t kSurvivalBeaconPeriodMs = 20000;
constexpr uint16_t kSurvivalSx1276AirtimeMs = 1712;

}  // namespace

void TxScheduler::begin_cycle(const TelemetryPacket& candidate, uint32_t cycle_start_ms) {
    const uint16_t sequence = sequence_++;

    slot_.packet          = candidate;
    slot_.packet.radio    = Radio::Sx1276;
    slot_.packet.sequence = sequence;
    slot_.due_ms          = cycle_start_ms;
    slot_.pending         = true;
}

bool TxScheduler::fits_duty_ceiling(uint32_t t_ms) {
    const RadioBudget budget = budget_for();

    // Expira o que já saiu pela traseira da janela.
    while (window_.count > 0 &&
           reached(t_ms, window_.entries[window_.head].t_ms + config_.duty_window_ms)) {
        window_.head = static_cast<uint8_t>((window_.head + 1) % kDutyWindowEntries);
        --window_.count;
    }

    // Anel cheio: recusa. Não pode acontecer enquanto o teto morder primeiro
    // (ver kDutyWindowEntries), e se acontecer o erro é para o lado seguro.
    if (window_.count >= kDutyWindowEntries) {
        return false;
    }

    uint32_t used_ms = 0;
    for (uint8_t i = 0; i < window_.count; ++i) {
        const uint8_t index = static_cast<uint8_t>((window_.head + i) % kDutyWindowEntries);
        used_ms += window_.entries[index].airtime_ms;
    }

    const uint32_t ceiling_ms = config_.duty_window_ms * budget.duty_permille / 1000u;
    return used_ms + budget.airtime_ms <= ceiling_ms;
}

void TxScheduler::charge_duty(uint32_t t_ms) {
    const uint8_t tail = static_cast<uint8_t>((window_.head + window_.count) % kDutyWindowEntries);

    window_.entries[tail].t_ms       = t_ms;
    window_.entries[tail].airtime_ms = budget_for().airtime_ms;
    ++window_.count;
}

uint32_t TxScheduler::cycle_period_ms() const {
    return config_.mode == TxMode::SurvivalBeacon ? kSurvivalBeaconPeriodMs
                                                   : config_.cycle_period_ms;
}

RadioBudget TxScheduler::budget_for() const {
    RadioBudget budget = config_.budget;
    if (config_.mode == TxMode::SurvivalBeacon) {
        budget.airtime_ms = kSurvivalSx1276AirtimeMs;
    }
    return budget;
}

TxSchedulerDecision TxScheduler::update(const TxSchedulerInput& input, uint32_t t_ms) {
    TxSchedulerDecision decision;

    // A época do ciclo é ancorada no primeiro update e daí em diante os
    // instantes saem de `início + n × período`, nunca de "última transmissão +
    // período": o jitter de uma volta do laço não pode se acumular.
    if (!started_) {
        started_       = true;
        next_cycle_ms_ = t_ms;
    }
    if (reached(t_ms, next_cycle_ms_)) {
        // Atrasado mais de um período inteiro significa que o laço ficou parado —
        // travada de cartão, task preemptada, watchdog. Os ciclos perdidos são
        // perdidos e a época é reancorada no presente: recuperá-los queimaria uma
        // sequência por volta do laço até alcançar o presente, e quase nenhuma
        // dessas transmissões passaria pela separação mínima. O buraco que a
        // estação de solo veria na sequência mediria perda que nunca existiu.
        const uint32_t period_ms = cycle_period_ms();
        const bool     stalled  = reached(t_ms, next_cycle_ms_ + period_ms);
        const uint32_t start_ms = stalled ? t_ms : next_cycle_ms_;

        begin_cycle(input.candidate, start_ms);
        next_cycle_ms_ = start_ms + period_ms;
    }

    if (slot_.pending && reached(t_ms, slot_.due_ms)) {
        // Escrita em andamento tem o barramento na mão. A transmissão espera —
        // adiada, não descartada — em vez de interromper a escrita: o registro
        // meio gravado é que custaria o arquivo inteiro.
        //
        // A separação mínima é verificada contra a última transmissão real. Um
        // slot barrado aqui é ADIADO, não descartado: ele continua pendente e sai
        // no primeiro tique em que a separação for satisfeita.
        //
        // O teto de ciclo de trabalho é a última palavra: a cadência pode pedir o
        // que quiser, o PA e a bateria é que pagam.
        const bool bus_busy = input.write_in_progress;
        const bool too_soon =
            has_transmitted_ && !reached(t_ms, last_tx_ms_ + config_.min_gap_ms);

        if (!bus_busy && !too_soon && fits_duty_ceiling(t_ms)) {
            charge_duty(t_ms);
            decision.transmissions[decision.transmission_count++] = slot_.packet;
            slot_.pending = false;

            has_transmitted_ = true;
            last_tx_ms_      = t_ms;
        }
    }

    return decision;
}

}  // namespace core
