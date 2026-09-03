#include "core/flight_phase.h"

#include <cmath>

#include "core/altitude.h"

namespace core {

void PhaseEstimator::begin(const PhaseRestore& restore) {
    phase_ = FlightPhase::Rail;
    has_reference_ = false;
    reference_pa_ = 0.0f;
    altitude_m_ = 0.0f;
    altitude_from_gps_ = false;
    has_liftoff_ = false;
    liftoff_ms_ = 0;
    above_threshold_ = false;
    has_stable_anchor_ = false;
    has_last_update_ = false;
    history_count_ = 0;
    history_head_ = 0;
    reused_ = false;
    reuse_sanity_pending_ = false;

    // Reuso só se o reset NÃO foi power-on limpo E a fase salva era de voo. Sem o
    // segundo teste, um brown-out na rampa faria o foguete voar com referência
    // velha; sem o primeiro, um power-on limpo herdaria a referência de outro voo.
    if (restore.have_saved && !restore.clean_poweron &&
        restore.saved.phase == FlightPhase::Flight) {
        phase_ = FlightPhase::Flight;
        reference_pa_ = restore.saved.reference_pa;
        has_reference_ = restore.saved.has_reference;
        liftoff_ms_ = restore.saved.liftoff_ms;
        has_liftoff_ = true;
        reused_ = true;
        reuse_sanity_pending_ = true;  // checada na primeira leitura de barômetro
    }
}

PhaseState PhaseEstimator::update(const SensorSample& sample, uint32_t t_ms) {
    if (sample.baro_valid) {
        if (!has_reference_) {
            reference_pa_ = sample.pressure_pa;
            has_reference_ = true;
        } else if (phase_ == FlightPhase::Rail) {
            // Média lenta contínua enquanto na rampa. A referência congela no
            // liftoff, então esta EWMA só corre em RAMPA. O passo é o Δt entre
            // amostras de barômetro (25 Hz), não o ciclo (100 Hz).
            const uint32_t dt = has_last_update_ ? (t_ms - last_update_ms_) : 0;
            float alpha = config_.reference_tau_ms > 0.0f
                              ? static_cast<float>(dt) / config_.reference_tau_ms
                              : 1.0f;
            if (alpha > 1.0f) {
                alpha = 1.0f;
            }
            reference_pa_ += (sample.pressure_pa - reference_pa_) * alpha;
        }
        record_pressure(sample.pressure_pa, t_ms);
        last_update_ms_ = t_ms;
        has_last_update_ = true;

        // Sanidade H4.4, uma vez, na primeira leitura depois de reusar: se a
        // referência reusada implica uma altitude absurda, o snapshot é de outro
        // voo ou de outro local. Cai para a altitude do GPS e sinaliza no pacote.
        if (reuse_sanity_pending_) {
            reuse_sanity_pending_ = false;
            const float implied =
                altitude_from_pressure(sample.pressure_pa, reference_pa_);
            if (std::fabs(implied) > config_.absurd_altitude_m) {
                altitude_from_gps_ = true;
            }
        }

        if (altitude_from_gps_ && sample.gps.valid) {
            altitude_m_ = sample.gps.altitude_m;
        } else {
            altitude_m_ = altitude_from_pressure(sample.pressure_pa, reference_pa_);
        }
    }

    bool persist_now = false;

    // Liftoff: só faz sentido na rampa, e é por |a| sustentado acima do limiar.
    if (phase_ == FlightPhase::Rail) {
        const float ax = static_cast<float>(sample.accel_mg[0]);
        const float ay = static_cast<float>(sample.accel_mg[1]);
        const float az = static_cast<float>(sample.accel_mg[2]);
        const float magnitude_mg = std::sqrt(ax * ax + ay * ay + az * az);

        if (magnitude_mg > static_cast<float>(config_.liftoff_accel_mg)) {
            if (!above_threshold_) {
                above_threshold_ = true;
                above_since_ms_ = t_ms;
            } else if (t_ms - above_since_ms_ >= config_.liftoff_sustain_ms) {
                // Liftoff. Congela a referência no valor de ~1 s ANTES, lido do
                // anel: o instante do liftoff já está contaminado pelo transiente
                // do motor. Só depois de congelar o campo passa a valer.
                reference_pa_ = pressure_at(t_ms - config_.freeze_lookback_ms);
                has_reference_ = true;
                phase_ = FlightPhase::Flight;
                has_liftoff_ = true;
                liftoff_ms_ = t_ms;
                persist_now = true;
            }
        } else {
            above_threshold_ = false;
        }
    }

    // Pouso: as QUATRO condições simultâneas. Nenhuma sozinha, porque em queda
    // livre no apogeu o acelerômetro lê ≈0 g de forma estável e sob paraquedas em
    // velocidade terminal lê ≈1 g — só a combinação de |a|≈1 g, altitude parada,
    // altitude perto do solo e tempo mínimo de voo distingue repouso de descida.
    if (phase_ == FlightPhase::Flight) {
        const float ax = static_cast<float>(sample.accel_mg[0]);
        const float ay = static_cast<float>(sample.accel_mg[1]);
        const float az = static_cast<float>(sample.accel_mg[2]);
        const float magnitude_mg = std::sqrt(ax * ax + ay * ay + az * az);

        const bool near_one_g =
            std::fabs(magnitude_mg - 1000.0f) < static_cast<float>(config_.landed_accel_tol_mg);

        // Estabilidade de altitude: uma âncora que só se mantém enquanto a altitude
        // não sai de uma faixa. Na descida a altitude cai sem parar e a âncora
        // reancora sem parar, então o cronômetro nunca completa.
        if (!has_stable_anchor_ || std::fabs(altitude_m_ - stable_anchor_m_) > config_.landed_alt_band_m) {
            has_stable_anchor_ = true;
            stable_anchor_m_ = altitude_m_;
            stable_since_ms_ = t_ms;
        }
        const bool altitude_stable =
            (t_ms - stable_since_ms_) >= config_.landed_alt_stable_ms;
        const bool near_ground = std::fabs(altitude_m_) < config_.landed_near_ground_m;
        const bool long_enough = (t_ms - liftoff_ms_) >= config_.landed_min_flight_ms;

        if (near_one_g && altitude_stable && near_ground && long_enough) {
            phase_ = FlightPhase::Landed;
            persist_now = true;
        }
    }

    PhaseState state;
    state.phase = phase_;
    state.has_reference = has_reference_;
    state.reference_pa = reference_pa_;
    state.altitude_m = altitude_m_;
    state.has_liftoff = has_liftoff_;
    state.liftoff_ms = liftoff_ms_;
    state.altitude_from_gps = altitude_from_gps_;
    state.persist_now = persist_now;
    return state;
}

PhaseSnapshot PhaseEstimator::snapshot() const {
    PhaseSnapshot snap;
    snap.phase = phase_;
    snap.reference_pa = reference_pa_;
    snap.liftoff_ms = liftoff_ms_;
    snap.has_reference = has_reference_;
    return snap;
}

void PhaseEstimator::record_pressure(float pressure_pa, uint32_t t_ms) {
    history_[history_head_] = {t_ms, pressure_pa};
    history_head_ = static_cast<uint8_t>((history_head_ + 1) % kHistory);
    if (history_count_ < kHistory) {
        ++history_count_;
    }
}

float PhaseEstimator::pressure_at(uint32_t target_ms) const {
    float    best = reference_pa_;
    uint32_t best_dist = UINT32_MAX;
    for (uint8_t i = 0; i < history_count_; ++i) {
        const Sample& s = history_[i];
        const uint32_t dist =
            s.t_ms > target_ms ? s.t_ms - target_ms : target_ms - s.t_ms;
        if (dist < best_dist) {
            best_dist = dist;
            best = s.pressure_pa;
        }
    }
    return best;
}

}  // namespace core
