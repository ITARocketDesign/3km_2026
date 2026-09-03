// core/stack_health.h — o limiar de folga de stack do diagnóstico (issue 11).
//
// Esta é a fronteira ÚNICA do "512 B". O firmware amostra o watermark de cada
// task com uxTaskGetStackHighWaterMark() (em bytes, na IDF), grava o valor bruto
// no registro do cartão (offsets 58 e 60), e usa ESTE limiar para decidir se a
// task está DEGRADED. As ferramentas de pós-voo (recover_log, harness da issue
// 14) leem o mesmo campo bruto contra o mesmo limiar. Um número duplicado nos
// dois lados divergiria com o tempo; um só, não.
//
// É diagnóstico só-de-log: NÃO vai ao pacote de rádio (não cabe nos 20 B) nem ao
// bitmap de saúde de subsistemas da issue 10 (que não tem conceito de task). O
// que atravessa para o solo é o watermark bruto no registro, nada mais.
#pragma once

#include <stdint.h>

namespace core {

// Folga mínima de stack, em bytes, abaixo da qual a task é diagnosticada
// DEGRADED. Stacks de 8 KB (flight) e 12 KB (io); 512 B é a margem escolhida na
// issue 11 §14.
constexpr uint16_t kStackWatermarkMinBytes = 512;

// A menor folga já observada está abaixo do limiar? Watermark em bytes. O limiar
// é exclusivo: exatamente 512 B ainda é saudável.
inline bool stack_watermark_degraded(uint16_t watermark_bytes) {
    return watermark_bytes < kStackWatermarkMinBytes;
}

}  // namespace core
