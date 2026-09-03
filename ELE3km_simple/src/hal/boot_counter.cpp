#include "hal/boot_counter.h"

#include <Preferences.h>

namespace hal {
namespace {

constexpr char kNamespace[] = "ele3km";
constexpr char kKey[] = "boot";

}  // namespace

uint16_t BootCounter::next() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
        return 0;
    }

    const uint16_t value = static_cast<uint16_t>(prefs.getUShort(kKey, 0) + 1);
    prefs.putUShort(kKey, value);
    prefs.end();
    return value;
}

}  // namespace hal
