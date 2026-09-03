// Suíte do diagnóstico de folga de stack (issue 11). Roda no notebook:
// `pio test -e native`.
//
// O único pedaço puro da issue 11: o limiar de 512 B abaixo do qual uma task é
// diagnosticada DEGRADED. Tudo o mais da issue — armar o TWDT, amostrar o
// watermark, gravá-lo no registro — é cola de firmware que só se verifica no
// target. Aqui mora a fronteira única do "512 B", para que o firmware e as
// ferramentas de pós-voo (recover_log, harness da issue 14) leiam o watermark
// bruto do registro contra o MESMO limiar, e não contra dois números que
// divergem com o tempo.
//
// O watermark é a MENOR folga de stack já observada, em bytes (é o que a IDF
// devolve). "Abaixo de 512" é degradado; exatamente 512 ainda não é.
#include <unity.h>

#include "core/stack_health.h"

using namespace core;

void setUp(void) {}
void tearDown(void) {}

// Folga confortável nos stacks de 8 KB e 12 KB: não é degradado.
void test_comfortable_headroom_is_not_degraded() {
    TEST_ASSERT_FALSE(stack_watermark_degraded(8192));
    TEST_ASSERT_FALSE(stack_watermark_degraded(1024));
}

// O limiar é "abaixo de 512", então 512 exatos ainda é saudável e 511 já não é.
void test_threshold_is_exclusive_at_512() {
    TEST_ASSERT_FALSE(stack_watermark_degraded(512));
    TEST_ASSERT_TRUE(stack_watermark_degraded(511));
}

// Folga mínima: um watermark que despencou para perto de zero é o caso que o
// diagnóstico existe para pegar.
void test_near_zero_headroom_is_degraded() {
    TEST_ASSERT_TRUE(stack_watermark_degraded(0));
    TEST_ASSERT_TRUE(stack_watermark_degraded(64));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_comfortable_headroom_is_not_degraded);
    RUN_TEST(test_threshold_is_exclusive_at_512);
    RUN_TEST(test_near_zero_headroom_is_degraded);
    return UNITY_END();
}
