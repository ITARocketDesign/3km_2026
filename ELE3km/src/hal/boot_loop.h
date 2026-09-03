// hal/boot_loop.h — historico de resets retido no dominio RTC (issue 12).
#pragma once

#include "core/boot_loop.h"

namespace hal {

// Chamado uma vez no boot, antes de inicializar o E22. Power-on limpo apaga o
// historico e sai do modo; qualquer outro reset acrescenta o carimbo do RTC.
core::BootLoopStatus detect_boot_loop_at_boot();

}  // namespace hal
