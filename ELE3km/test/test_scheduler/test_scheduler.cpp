// Suíte do escalonador de arbitragem. Roda no notebook: `pio test -e native`.
//
// O escalonador é o seam que torna as invariantes de recurso testáveis (PRD §20)
// — é a razão de ele viver no núcleo e não dentro da task. Com um único rádio (o
// SX1276 onboard; o E22 foi removido — Docs/ELE3km_drop_e22_single_radio.md) o que
// se verifica aqui é: uma transmissão por ciclo, o teto de ciclo de trabalho nunca
// furado, e a escrita no cartão nunca interrompida por uma transmissão.
//
// Nenhum teste inspeciona estado interno — só o que sai do escalonador.
#include <unity.h>

#include "core/tx_scheduler.h"

using namespace core;

namespace {

// Uma transmissão observada: o que saiu e quando.
struct Emission {
    uint32_t        t_ms;
    TelemetryPacket packet;
};

constexpr int kMaxEmissions = 256;

TelemetryPacket empty_candidate(uint32_t) {
    return TelemetryPacket();
}

// Roda o escalonador de 0 a `until_ms` no passo de amostragem do voo (25 Hz, a
// taxa do barômetro) e coleta tudo o que ele liberou. `candidate_at` é o pacote
// que o FlightComputer teria montado naquele instante.
int run(TxScheduler& scheduler, uint32_t until_ms, Emission* out,
        TelemetryPacket (*candidate_at)(uint32_t) = empty_candidate, uint32_t step_ms = 40) {
    int count = 0;
    for (uint32_t t_ms = 0; t_ms <= until_ms; t_ms += step_ms) {
        TxSchedulerInput input;
        input.candidate = candidate_at(t_ms);

        const TxSchedulerDecision decision = scheduler.update(input, t_ms);
        for (uint8_t i = 0; i < decision.transmission_count && count < kMaxEmissions; ++i) {
            out[count].t_ms   = t_ms;
            out[count].packet = decision.transmissions[i];
            ++count;
        }
    }
    return count;
}

}  // namespace

// Um único rádio transmite uma vez por ciclo de telemetria, desde o boot, pelo
// SX1276. Num segundo (1 Hz) sai exatamente uma transmissão, em t = 0.
void test_one_packet_per_cycle_at_1hz(void) {
    TxScheduler scheduler;
    Emission    emissions[kMaxEmissions];

    const int count = run(scheduler, 999, emissions);

    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Radio::Sx1276),
                            static_cast<uint8_t>(emissions[0].packet.radio));
    TEST_ASSERT_EQUAL_UINT32(0, emissions[0].t_ms);
}

// O número de sequência anda uma vez por CICLO DE TELEMETRIA. É por ele que a
// estação de solo ordena e deduplica depois que o campo de tempo satura.
void test_sequence_advances_once_per_cycle(void) {
    TxScheduler scheduler;
    Emission    emissions[kMaxEmissions];

    // Cinco ciclos completos (1 Hz de t = 0 a t = 4999).
    const int count = run(scheduler, 4999, emissions);

    TEST_ASSERT_EQUAL_INT(5, count);
    for (int i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(i), emissions[i].packet.sequence);
    }
}

// Perfil em que o conteúdo muda a cada tique de amostragem: um metro por
// decissegundo. O escalonador latcheia o candidato no INÍCIO do ciclo — é essa
// cópia que vai ao ar, não a do instante em que a transmissão é liberada.
TelemetryPacket climbing_candidate(uint32_t t_ms) {
    TelemetryPacket packet;
    packet.altitude_m = static_cast<int16_t>(t_ms / 10);
    return packet;
}

void test_packet_content_is_latched_at_cycle_start(void) {
    TxScheduler scheduler;
    Emission    emissions[kMaxEmissions];

    const int count = run(scheduler, 1999, emissions, climbing_candidate);

    TEST_ASSERT_EQUAL_INT(2, count);
    // Ciclo 0 latcheado em t = 0; ciclo 1 em t = 1000 (altitude 100).
    TEST_ASSERT_EQUAL_INT16(0, emissions[0].packet.altitude_m);
    TEST_ASSERT_EQUAL_INT16(100, emissions[1].packet.altitude_m);
}

// O teto de ciclo de trabalho nunca é furado, SEJA QUAL FOR o que a lógica de
// cadência peça (PRD §65, hazard H1c). Aqui ela pede quatro ciclos por segundo —
// o que a fase pós-pouso ou um bug de cadência produziriam — e o escalonador
// recusa o que não cabe no orçamento.
//
// A verificação é feita como janela deslizante de verdade: para CADA transmissão
// liberada, soma-se o airtime de tudo o que o rádio pôs no ar na janela que
// termina nela. É a pior janela possível, e é a que o PA e o rail sentem.
void test_duty_ceiling_holds_when_the_cadence_asks_for_more(void) {
    TxSchedulerConfig config;
    config.cycle_period_ms = 250;  // 4 Hz: muito acima do que o orçamento paga

    TxScheduler scheduler(config);
    Emission    emissions[kMaxEmissions];

    const int count = run(scheduler, 19999, emissions);  // duas janelas cheias

    // Um escalonador que não transmitisse nada também respeitaria o teto.
    TEST_ASSERT_GREATER_THAN_INT(0, count);

    const uint32_t airtime = config.budget.airtime_ms;
    for (int i = 0; i < count; ++i) {
        uint32_t used_ms = 0;
        for (int j = 0; j <= i; ++j) {
            if (emissions[i].t_ms - emissions[j].t_ms >= config.duty_window_ms) continue;
            used_ms += airtime;
        }
        const uint32_t ceiling_ms =
            config.duty_window_ms * config.budget.duty_permille / 1000u;
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(ceiling_ms, used_ms);
    }
}

// A separação mínima é um backstop VERIFICADO (PRD §5): mesmo que a cadência peça
// ciclos mais próximos que min_gap, duas transmissões liberadas nunca saem mais
// perto que isso. Aqui os ciclos são de 100 ms e a folga de orçamento é ampla, de
// forma que quem morde é a separação mínima, não o teto de ciclo de trabalho.
void test_min_gap_separates_transmissions(void) {
    TxSchedulerConfig config;
    config.cycle_period_ms = 100;      // 10 Hz nominal
    config.budget          = {10, 1000};  // airtime baixo, teto de 100 %: o duty não morde
    config.min_gap_ms      = 200;

    TxScheduler scheduler(config);
    Emission    emissions[kMaxEmissions];

    const int count = run(scheduler, 2000, emissions, empty_candidate, 20);

    TEST_ASSERT_GREATER_THAN_INT(1, count);
    for (int i = 1; i < count; ++i) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(config.min_gap_ms,
                                            emissions[i].t_ms - emissions[i - 1].t_ms);
    }
}

// Um número de sequência só é gasto por um ciclo que teve chance de transmitir.
//
// Se o laço ficar parado vários períodos — travada de cartão, task preemptada — os
// ciclos perdidos ficam perdidos. O que NÃO pode acontecer é o escalonador tentar
// recuperá-los numa rajada: ele queimaria uma sequência a cada volta do laço até
// alcançar o presente, e a estação de solo veria um salto na sequência e concluiria
// que perdeu pacotes que nunca foram ao ar.
void test_a_stalled_loop_does_not_burn_a_burst_of_cycles(void) {
    TxScheduler      scheduler;
    TxSchedulerInput input;

    // Um ciclo normal, e então o laço some por cinco segundos.
    scheduler.update(input, 0);

    uint16_t sequences[kMaxEmissions];
    int      count = 0;
    for (uint32_t t_ms = 5000; t_ms <= 8000; t_ms += 40) {
        const TxSchedulerDecision decision = scheduler.update(input, t_ms);
        for (uint8_t i = 0; i < decision.transmission_count && count < kMaxEmissions; ++i) {
            sequences[count++] = decision.transmissions[i].sequence;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT(0, count);
    for (int i = 1; i < count; ++i) {
        // Cada ciclo anda exatamente um. Qualquer salto maior é sequência queimada
        // sem transmissão.
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(1, sequences[i] - sequences[i - 1]);
    }
}

// Uma solicitação de transmissão ESPERA uma escrita em andamento, nunca a
// interrompe. A disputa é pelo barramento SPI, que a escrita já tem na mão e que
// carregar a carga útil do rádio também precisa.
//
// E esperar não é desistir: o pacote sai quando a escrita termina, com o número
// de sequência do ciclo dele. Descartá-lo seria trocar um buraco no log por um
// buraco no link de recuperação, que é a troca errada.
void test_a_transmission_waits_for_a_write_instead_of_interrupting_it(void) {
    TxScheduler      scheduler;
    TxSchedulerInput input;

    // Escrita em andamento durante toda a janela em que o slot do ciclo vence.
    input.write_in_progress = true;
    for (uint32_t t_ms = 0; t_ms <= 200; t_ms += 20) {
        const TxSchedulerDecision decision = scheduler.update(input, t_ms);
        TEST_ASSERT_EQUAL_UINT8(0, decision.transmission_count);
    }

    // A escrita termina.
    input.write_in_progress = false;
    const TxSchedulerDecision decision = scheduler.update(input, 220);

    TEST_ASSERT_EQUAL_UINT8(1, decision.transmission_count);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Radio::Sx1276),
                            static_cast<uint8_t>(decision.transmissions[0].radio));
    TEST_ASSERT_EQUAL_UINT16(0, decision.transmissions[0].sequence);
}

// O modo de sobrevivencia troca a cadencia: o SX1276 assume sozinho o beacon de
// 20 s em SF12. (Ja era so o SX1276 antes de o E22 sair; agora e o unico radio em
// qualquer modo.)
void test_survival_beacon_emits_every_twenty_seconds(void) {
    TxSchedulerConfig config;
    config.mode = TxMode::SurvivalBeacon;

    TxScheduler scheduler(config);
    Emission    emissions[kMaxEmissions];

    const int count = run(scheduler, 40'000, emissions);

    TEST_ASSERT_EQUAL_INT(3, count);
    for (int i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Radio::Sx1276),
                                static_cast<uint8_t>(emissions[i].packet.radio));
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i) * 20'000u, emissions[i].t_ms);
    }
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_one_packet_per_cycle_at_1hz);
    RUN_TEST(test_sequence_advances_once_per_cycle);
    RUN_TEST(test_packet_content_is_latched_at_cycle_start);
    RUN_TEST(test_duty_ceiling_holds_when_the_cadence_asks_for_more);
    RUN_TEST(test_min_gap_separates_transmissions);
    RUN_TEST(test_a_stalled_loop_does_not_burn_a_burst_of_cycles);
    RUN_TEST(test_a_transmission_waits_for_a_write_instead_of_interrupting_it);
    RUN_TEST(test_survival_beacon_emits_every_twenty_seconds);
    return UNITY_END();
}
