#include "core/boot_loop.h"

namespace core {

BootLoopStatus detect_boot_loop(const uint64_t* reset_timestamps_us, uint8_t count) {
    BootLoopStatus status;
    if (reset_timestamps_us == nullptr || count == 0) {
        return status;
    }

    const uint64_t newest_us = reset_timestamps_us[count - 1];
    for (uint8_t i = 0; i < count; ++i) {
        const uint64_t timestamp_us = reset_timestamps_us[i];
        if (timestamp_us <= newest_us && newest_us - timestamp_us < kBootLoopWindowUs) {
            ++status.recent_reset_count;
        }
    }
    status.active = status.recent_reset_count >= kBootLoopResetThreshold;
    return status;
}

}  // namespace core
