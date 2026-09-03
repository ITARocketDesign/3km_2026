// Suíte da máquina de saúde por subsistema (issue 10). Roda no notebook:
// `pio test -e native`.
//
// A parte pura da issue 10: a máquina de estados {OK, DEGRADED, FAILED} uniforme,
// a retentativa de período fixo de 5 s, a reverificação periódica dos
// registradores de configuração, e o contador de reinicializações por sensor. A
// cola de firmware — o timeout duro de cada driver, a leitura real do registrador
// de configuração, o carimbo no Serial — só se verifica no target; aqui mora só a
// política de tempo e de transição, testada pelo comportamento externo.
//
// Nenhum teste inspeciona estado interno — só o estado exposto e os pedidos de
// ação que a máquina faz ao chamador.
#include <unity.h>

#include "core/health.h"

using namespace core;

void setUp(void) {}
void tearDown(void) {}

// Um subsistema presente no startup começa saudável, sem reinicializações.
void test_present_at_startup_is_ok(void) {
    SubsystemHealth health;
    health.begin(/*present=*/true, /*now_ms=*/0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HealthState::Ok),
                            static_cast<uint8_t>(health.state()));
    TEST_ASSERT_TRUE(health.ok());
    TEST_ASSERT_EQUAL_UINT16(0, health.reinit_count());
}

// Um subsistema ausente no startup começa FAILED — e é dele que a retentativa
// periódica cuida a partir daí.
void test_absent_at_startup_is_failed(void) {
    SubsystemHealth health;
    health.begin(/*present=*/false, /*now_ms=*/0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HealthState::Failed),
                            static_cast<uint8_t>(health.state()));
    TEST_ASSERT_FALSE(health.ok());
}

// AC: módulo em FAILED retentado EXATAMENTE a cada 5 s, período fixo,
// indefinidamente. A retentativa não está pronta antes dos 5 s; está pronta no
// instante exato; e uma tentativa malsucedida reagenda a próxima para +5 s.
void test_failed_module_retried_exactly_every_5s(void) {
    SubsystemHealth health;
    health.begin(/*present=*/false, /*now_ms=*/0);

    TEST_ASSERT_FALSE(health.recovery_due(4999));
    TEST_ASSERT_TRUE(health.recovery_due(5000));

    // A tentativa falhou: continua FAILED e a próxima cai em +5 s, não antes.
    health.observe_recovery(/*ok=*/false, /*now_ms=*/5000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HealthState::Failed),
                            static_cast<uint8_t>(health.state()));
    TEST_ASSERT_FALSE(health.recovery_due(9999));
    TEST_ASSERT_TRUE(health.recovery_due(10000));

    // E indefinidamente: mais uma falha, mais 5 s.
    health.observe_recovery(/*ok=*/false, /*now_ms=*/10000);
    TEST_ASSERT_FALSE(health.recovery_due(14999));
    TEST_ASSERT_TRUE(health.recovery_due(15000));
}

// AC: uma retentativa bem-sucedida volta o módulo para OK, e cada reinicialização
// bem-sucedida sobe o contador por sensor — o número que, se subir durante o voo,
// denuncia o rail afundando.
void test_successful_recovery_returns_to_ok_and_counts_reinit(void) {
    SubsystemHealth health;
    health.begin(/*present=*/false, /*now_ms=*/0);

    health.observe_recovery(/*ok=*/true, /*now_ms=*/5000);
    TEST_ASSERT_TRUE(health.ok());
    TEST_ASSERT_EQUAL_UINT16(1, health.reinit_count());

    // Cai de novo e volta de novo: o contador acumula, não zera na volta ao OK.
    health.report_operation(/*ok=*/false, /*now_ms=*/6000);
    TEST_ASSERT_FALSE(health.ok());
    health.observe_recovery(/*ok=*/true, /*now_ms=*/11000);
    TEST_ASSERT_TRUE(health.ok());
    TEST_ASSERT_EQUAL_UINT16(2, health.reinit_count());
}

// AC (o ponto menos óbvio da issue): um sensor que fez brown-out e voltou
// responde normalmente e devolve dados PLAUSÍVEIS com a escala errada. As
// operações de dados seguem passando; só a reverificação dos registradores de
// CONFIGURAÇÃO detecta o desvio. A reverificação é periódica (mesma cadência de
// 5 s), e uma configuração que não bate leva o módulo a DEGRADED — apesar de as
// leituras de dados estarem "boas".
void test_config_mismatch_detected_by_reverify_not_data(void) {
    SubsystemHealth health;
    health.begin(/*present=*/true, /*now_ms=*/0);

    // As leituras de dados continuam passando o tempo todo.
    health.report_operation(/*ok=*/true, /*now_ms=*/1000);
    health.report_operation(/*ok=*/true, /*now_ms=*/3000);

    // A reverificação de configuração é periódica: não antes de 5 s, sim aos 5 s.
    TEST_ASSERT_FALSE(health.config_verify_due(4999));
    TEST_ASSERT_TRUE(health.config_verify_due(5000));

    // A configuração relida não bate: fábrica, não a escala que begin() aplicou.
    // Mesmo com as leituras de dados "boas", o módulo cai para DEGRADED.
    health.report_config(/*matches=*/false, /*now_ms=*/5000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HealthState::Degraded),
                            static_cast<uint8_t>(health.state()));
    TEST_ASSERT_FALSE(health.ok());
}

// Uma configuração que bate mantém o módulo OK e apenas reinicia a cadência de
// reverificação para a próxima janela.
void test_config_match_keeps_ok_and_reschedules(void) {
    SubsystemHealth health;
    health.begin(/*present=*/true, /*now_ms=*/0);

    TEST_ASSERT_TRUE(health.config_verify_due(5000));
    health.report_config(/*matches=*/true, /*now_ms=*/5000);
    TEST_ASSERT_TRUE(health.ok());

    // Verificou agora; a próxima janela é +5 s, não imediatamente.
    TEST_ASSERT_FALSE(health.config_verify_due(9999));
    TEST_ASSERT_TRUE(health.config_verify_due(10000));
}

// Um módulo DEGRADED (voltou com configuração de fábrica) é reinicializado pela
// mesma cadência de 5 s — o begin() reaplica a escala correta — e volta a OK.
// Reconfigurar é uma reinicialização, então o contador sobe.
void test_degraded_module_reinits_on_the_5s_cadence(void) {
    SubsystemHealth health;
    health.begin(/*present=*/true, /*now_ms=*/0);
    health.report_config(/*matches=*/false, /*now_ms=*/5000);  // → DEGRADED

    TEST_ASSERT_FALSE(health.recovery_due(9999));
    TEST_ASSERT_TRUE(health.recovery_due(10000));

    health.observe_recovery(/*ok=*/true, /*now_ms=*/10000);
    TEST_ASSERT_TRUE(health.ok());
    TEST_ASSERT_EQUAL_UINT16(1, health.reinit_count());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_present_at_startup_is_ok);
    RUN_TEST(test_absent_at_startup_is_failed);
    RUN_TEST(test_failed_module_retried_exactly_every_5s);
    RUN_TEST(test_successful_recovery_returns_to_ok_and_counts_reinit);
    RUN_TEST(test_config_mismatch_detected_by_reverify_not_data);
    RUN_TEST(test_config_match_keeps_ok_and_reschedules);
    RUN_TEST(test_degraded_module_reinits_on_the_5s_cadence);
    return UNITY_END();
}
