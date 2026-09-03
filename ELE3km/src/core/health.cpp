#include "core/health.h"

namespace core {

void SubsystemHealth::begin(bool present, uint32_t now_ms) {
    if (present) {
        state_          = HealthState::Ok;
        last_verify_ms_ = now_ms;
    } else {
        state_            = HealthState::Failed;
        next_recovery_ms_ = now_ms + kHealthRetryPeriodMs;
    }
}

void SubsystemHealth::report_operation(bool ok, uint32_t now_ms) {
    if (!ok) {
        state_            = HealthState::Failed;
        next_recovery_ms_ = now_ms + kHealthRetryPeriodMs;
    }
}

bool SubsystemHealth::recovery_due(uint32_t now_ms) const {
    if (state_ == HealthState::Ok) return false;
    // Comparação segura contra o wrap de millis(), como no resto do firmware.
    return static_cast<int32_t>(now_ms - next_recovery_ms_) >= 0;
}

void SubsystemHealth::observe_recovery(bool ok, uint32_t now_ms) {
    if (ok) {
        state_          = HealthState::Ok;
        last_verify_ms_ = now_ms;
        if (reinit_count_ != 0xFFFFu) ++reinit_count_;  // satura: diagnóstico, não conta exata
    } else {
        next_recovery_ms_ = now_ms + kHealthRetryPeriodMs;
    }
}

bool SubsystemHealth::config_verify_due(uint32_t now_ms) const {
    if (state_ != HealthState::Ok) return false;
    return static_cast<int32_t>(now_ms - last_verify_ms_) >= static_cast<int32_t>(kHealthRetryPeriodMs);
}

void SubsystemHealth::report_config(bool matches, uint32_t now_ms) {
    if (matches) {
        last_verify_ms_ = now_ms;
    } else {
        state_            = HealthState::Degraded;
        next_recovery_ms_ = now_ms + kHealthRetryPeriodMs;
    }
}

}  // namespace core
