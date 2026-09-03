#include "core/altitude.h"

#include <cmath>

namespace core {

float altitude_from_pressure(float pressure_pa, float reference_pa) {
    if (!(pressure_pa > 0.0f) || !(reference_pa > 0.0f)) {
        return 0.0f;
    }
    // h = (T0/L) · [1 − (p/p0)^(R·L/(g·M))], com T0 = 288,15 K e L = 6,5 K/km.
    return 44330.0f * (1.0f - std::pow(pressure_pa / reference_pa, 0.1902949f));
}

}  // namespace core
