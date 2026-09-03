#include "hal/boot_loop.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <soc/esp32/rtc.h>
#include <string.h>

namespace hal {
namespace {

constexpr uint32_t kHistoryMagic = 0xE13B0012u;
constexpr uint8_t  kHistoryCapacity = core::kBootLoopResetThreshold;

struct RetainedBootHistory {
    uint32_t magic;
    uint64_t timestamps_us[kHistoryCapacity];
    uint8_t  count;
    bool     survival_latched;
    uint8_t  trigger_reset_count;
};

// Diferente de RTC_DATA_ATTR, RTC_NOINIT_ATTR nao e reinicializado depois de
// restart. Power-on e tratado explicitamente abaixo; magic protege o primeiro
// boot caso o conteudo retido seja indeterminado.
RTC_NOINIT_ATTR RetainedBootHistory g_history;

void clear_history() {
    memset(&g_history, 0, sizeof(g_history));
    g_history.magic = kHistoryMagic;
}

void append_timestamp(uint64_t timestamp_us) {
    if (g_history.count < kHistoryCapacity) {
        g_history.timestamps_us[g_history.count++] = timestamp_us;
        return;
    }

    for (uint8_t i = 1; i < kHistoryCapacity; ++i) {
        g_history.timestamps_us[i - 1] = g_history.timestamps_us[i];
    }
    g_history.timestamps_us[kHistoryCapacity - 1] = timestamp_us;
}

}  // namespace

core::BootLoopStatus detect_boot_loop_at_boot() {
    if (esp_reset_reason() == ESP_RST_POWERON) {
        clear_history();
        return {};
    }
    if (g_history.magic != kHistoryMagic || g_history.count > kHistoryCapacity) {
        clear_history();
    }

    // Depois de disparado, so power-on limpo sai do modo. Nao se reavalia a
    // janela e nao se reabilita o E22 automaticamente depois de 30 s.
    if (g_history.survival_latched) {
        core::BootLoopStatus latched;
        latched.active = true;
        latched.recent_reset_count = g_history.trigger_reset_count;
        return latched;
    }

    append_timestamp(esp_rtc_get_time_us());
    core::BootLoopStatus status =
        core::detect_boot_loop(g_history.timestamps_us, g_history.count);
    if (status.active) {
        g_history.survival_latched = true;
        g_history.trigger_reset_count = status.recent_reset_count;
    }
    return status;
}

}  // namespace hal
