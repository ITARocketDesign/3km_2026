// core/boot_loop.h — detector puro de resets repetidos (issue 12).
//
// A origem do tempo e a memoria retida pertencem a HAL. O nucleo recebe apenas
// carimbos monotonicamente crescentes e devolve a decisao observavel, sem ler
// relogio global nem manter estado global.
#pragma once

#include <stdint.h>

namespace core {

constexpr uint64_t kBootLoopWindowUs = 30000000ULL;
constexpr uint8_t  kBootLoopResetThreshold = 3;

struct BootLoopStatus {
    bool    active = false;
    uint8_t recent_reset_count = 0;
};

// Conta os resets que pertencem a janela aberta de 30 s terminada no carimbo
// mais recente. A entrada esta em ordem cronologica.
BootLoopStatus detect_boot_loop(const uint64_t* reset_timestamps_us, uint8_t count);

}  // namespace core
