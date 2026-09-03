// Suíte da máquina de fases e da referência barométrica (issue 06). Roda no
// notebook: `pio test -e native`.
//
// O teste de maior valor da suíte inteira (PRD §22, hardware_constraints C2) mora
// aqui: um perfil sintético de voo completo — rampa → boost → coast → QUEDA LIVRE
// no apogeu → descida → toque — em que a fase tem que permanecer em VOO durante
// toda a queda livre. Queda livre lê ≈0 g de forma estável; repouso lê ≈1 g. Um
// detector de pouso por baixa variância confunde os dois e mata a telemetria da
// descida; essa distinção é o teste inteiro.
#include <unity.h>

#include <cmath>

#include "core/flight_phase.h"

using namespace core;

namespace {

// Uma amostra com pressão e módulo de aceleração dados. O acelerômetro em repouso
// lê 1 g em um eixo; para os testes basta pôr o módulo desejado num eixo.
SensorSample sample_at(float pressure_pa, uint16_t accel_mg) {
    SensorSample s;
    s.baro_valid  = true;
    s.pressure_pa = pressure_pa;
    s.accel_mg[0] = 0;
    s.accel_mg[1] = 0;
    s.accel_mg[2] = static_cast<int16_t>(accel_mg);
    return s;
}

constexpr float kSeaLevelPa = 101325.0f;
constexpr uint16_t kOneG = 1000;  // mg

}  // namespace

// Tracer: na rampa, parado, a fase é RAMPA e a referência já existe, então a
// altitude reportada fica em ~0. Prova o caminho antes de qualquer transição.
void test_on_the_pad_phase_is_rail_and_altitude_is_zero(void) {
    PhaseEstimator estimator;
    PhaseRestore restore;  // defaults: power-on limpo, sem snapshot
    estimator.begin(restore);

    const PhaseState state = estimator.update(sample_at(kSeaLevelPa, kOneG), 0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(state.phase));
    TEST_ASSERT_TRUE(state.has_reference);
    TEST_ASSERT_FALSE(state.has_liftoff);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, state.altitude_m);
}

// A referência na rampa é uma média LENTA (constante de tempo de dezenas de
// segundos), não a primeira leitura. Uma referência tirada no power-on já está
// velha na ignição: a atmosfera deriva e a eletrônica esquenta. O teste força as
// duas metades: logo depois de um degrau de pressão a referência mal se move (a
// altitude reflete quase todo o degrau), e só depois de dezenas de segundos ela
// alcança o novo ambiente (a altitude volta a ~0).
void test_pad_reference_is_a_slow_average(void) {
    PhaseConfig config;  // tau = 30 s
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    // Estabelece a referência no ambiente inicial.
    estimator.update(sample_at(kSeaLevelPa, kOneG), 0);

    // Degrau de pressão equivalente a ~+8 m, mantido. Parado (1 g), sem liftoff.
    const float stepped_pa = kSeaLevelPa - 100.0f;

    // Depois de 1 s de degrau, a referência mal andou: a altitude ainda mostra
    // quase todo o degrau. Uma referência que seguisse na hora daria ~0.
    uint32_t t = 0;
    PhaseState state;
    for (int i = 0; i < 25; ++i) {  // 1 s a 25 Hz
        t += 40;
        state = estimator.update(sample_at(stepped_pa, kOneG), t);
    }
    TEST_ASSERT_TRUE(state.altitude_m > 6.0f);  // ~8 m de degrau, quase inteiro

    // Mantido por ~90 s (3 τ), a referência alcança o novo ambiente e a altitude
    // volta para perto de zero. Uma referência de tiro único ficaria presa nos 8 m.
    for (int i = 0; i < 25 * 90; ++i) {
        t += 40;
        state = estimator.update(sample_at(stepped_pa, kOneG), t);
    }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, state.altitude_m);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(state.phase));
}

// Liftoff é por aceleração sustentada acima do limiar (~2,5 g por ~100 ms), nunca
// por altitude. Um pico curto do manuseio na rampa não pode disparar, e uma
// aceleração abaixo do limiar também não — só o boost sustentado.
void test_liftoff_needs_sustained_acceleration(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    uint32_t t = 0;
    estimator.update(sample_at(kSeaLevelPa, kOneG), t);

    // Um pico curto de 3 g por 40 ms: abaixo dos 100 ms, não é liftoff.
    t += 40;
    PhaseState state = estimator.update(sample_at(kSeaLevelPa, 3000), t);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(state.phase));

    // Volta a 1 g: o acumulador zera.
    t += 40;
    estimator.update(sample_at(kSeaLevelPa, kOneG), t);

    // Aceleração abaixo do limiar (2 g), sustentada: não é liftoff.
    for (int i = 0; i < 10; ++i) {
        t += 40;
        state = estimator.update(sample_at(kSeaLevelPa, 2000), t);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(state.phase));

    // Volta a 1 g.
    t += 40;
    estimator.update(sample_at(kSeaLevelPa, kOneG), t);

    // 3 g sustentado por mais de 100 ms: liftoff. A fase vira VOO, o liftoff é
    // marcado, e o instante é persistido.
    bool saw_persist = false;
    for (int i = 0; i < 6; ++i) {  // 240 ms
        t += 40;
        state = estimator.update(sample_at(kSeaLevelPa, 3000), t);
        saw_persist = saw_persist || state.persist_now;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(state.phase));
    TEST_ASSERT_TRUE(state.has_liftoff);
    TEST_ASSERT_TRUE(saw_persist);
}

// No liftoff a referência congelada é a de ~1 s antes, lida do anel — não a do
// instante do transiente. Quando o motor acende, a pressão no bay dá um transiente
// e a aceleração sobe ao mesmo tempo; congelar no instante do liftoff enfiaria
// esse transiente direto no zero da altitude, e o erro entra no apogeu.
void test_frozen_reference_is_the_value_from_a_second_before(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    // 3 s de pressão estável de solo: a referência assenta em kSeaLevelPa.
    uint32_t t = 0;
    for (int i = 0; i < 75; ++i) {
        estimator.update(sample_at(kSeaLevelPa, kOneG), t);
        t += 40;
    }

    // O motor acende: transiente de pressão (~-300 Pa) E 3 g ao mesmo tempo. Menos
    // de 1 s depois o liftoff é detectado.
    const float transient_pa = kSeaLevelPa - 300.0f;
    PhaseState state;
    for (int i = 0; i < 6; ++i) {  // 240 ms de transiente + boost
        state = estimator.update(sample_at(transient_pa, 3000), t);
        t += 40;
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(state.phase));
    // A referência congelada é a de solo, não a do transiente: perto de kSeaLevelPa
    // (dentro de ~4 m), longe dos -300 Pa do transiente (~-25 m).
    TEST_ASSERT_FLOAT_WITHIN(50.0f, kSeaLevelPa, state.reference_pa);
}

// Pressão de solo para uma altitude dada (inverte a fórmula ISA da troposfera).
float pressure_for_altitude(float altitude_m) {
    return kSeaLevelPa * std::pow(1.0f - altitude_m / 44330.0f, 5.255f);
}

// O TESTE DE MAIOR VALOR DA SUÍTE (PRD §22, C2). Um perfil sintético completo —
// rampa → boost → coast → QUEDA LIVRE no apogeu → descida → toque — e a fase tem
// que permanecer em VOO durante toda a queda livre do apogeu. Um detector de pouso
// por baixa variância dispararia no apogeu (0 g estável) e mataria a telemetria da
// descida inteira, que é a razão de existir do projeto.
void test_full_flight_profile_stays_in_flight_through_apogee(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    uint32_t t = 0;
    auto step = [&](float altitude_m, uint16_t accel_mg) -> PhaseState {
        const PhaseState s =
            estimator.update(sample_at(pressure_for_altitude(altitude_m), accel_mg), t);
        t += 40;  // 25 Hz
        return s;
    };

    // Rampa: 3 s parado no solo, 1 g. Assenta a referência.
    for (int i = 0; i < 75; ++i) {
        step(0.0f, kOneG);
    }

    // Boost: ~2 s de 6 g subindo de 0 a ~600 m.
    for (int i = 0; i < 50; ++i) {
        step(600.0f * (i + 1) / 50.0f, 6000);
    }
    PhaseState state = estimator.update(
        sample_at(pressure_for_altitude(600.0f), 6000), t);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(state.phase));

    // Coast até o apogeu a ~3000 m: motor apagado, ainda subindo, ~0 g (queda livre
    // balística já é 0 g). ~8 s.
    for (int i = 0; i < 200; ++i) {
        const float alt = 600.0f + (3000.0f - 600.0f) * (i + 1) / 200.0f;
        step(alt, 0);
    }

    // QUEDA LIVRE no apogeu: altitude quase parada no topo por ~3 s, 0 g ESTÁVEL.
    // É aqui que o detector ingênuo dispara. A fase tem que continuar em VOO.
    for (int i = 0; i < 75; ++i) {
        state = step(3000.0f, 0);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(FlightPhase::Flight),
                                        static_cast<uint8_t>(state.phase),
                                        "disparou pouso no apogeu (0 g estavel)");
    }

    // Descida sob paraquedas: ~1 g (velocidade terminal), altitude caindo de 3000 a
    // 0 ao longo de ~30 s. A altitude nunca para, então o pouso não fecha.
    for (int i = 0; i < 750; ++i) {
        const float alt = 3000.0f * (1.0f - (i + 1) / 750.0f);
        state = step(alt, kOneG);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(FlightPhase::Flight),
                                        static_cast<uint8_t>(state.phase),
                                        "declarou pouso durante a descida");
    }

    // Toque: parado no solo a 1 g, altitude estável perto de zero. Depois das
    // quatro condições, POUSADO.
    for (int i = 0; i < 400; ++i) {  // 16 s > os 10 s de estabilidade
        state = step(0.0f, kOneG);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(state.phase));
}

// Leva o estimador até POUSADO por um caminho curto: liftoff, espera o tempo
// mínimo de voo, e assenta no solo. Usado pelos dois testes seguintes.
PhaseEstimator landed_estimator(uint32_t& t_out) {
    PhaseConfig config;
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    uint32_t t = 0;
    for (int i = 0; i < 50; ++i) {  // rampa
        estimator.update(sample_at(kSeaLevelPa, kOneG), t);
        t += 40;
    }
    for (int i = 0; i < 6; ++i) {   // liftoff
        estimator.update(sample_at(kSeaLevelPa, 3000), t);
        t += 40;
    }
    // Passa o tempo mínimo de voo (20 s) parado no solo, 1 g, altitude ~0.
    PhaseState state;
    for (int i = 0; i < 750; ++i) {  // 30 s
        state = estimator.update(sample_at(kSeaLevelPa, kOneG), t);
        t += 40;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(state.phase));
    t_out = t;
    return estimator;
}

// POUSADO é de mão única: uma vez atingido, nem um pico de aceleração nem uma
// mudança de altitude o abandona. Só um reset sai dele. Um falso negativo custa
// bateria; um falso positivo custa o voo, então a saída é fechada.
void test_landed_is_one_way(void) {
    uint32_t t = 0;
    PhaseEstimator estimator = landed_estimator(t);

    // Chuta o sistema: 8 g e altitude subindo de novo. Nada disso sai de POUSADO.
    for (int i = 0; i < 100; ++i) {
        const PhaseState s = estimator.update(sample_at(kSeaLevelPa - 5000.0f, 8000), t);
        t += 40;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                                static_cast<uint8_t>(s.phase));
    }
}

// As quatro condições são exigidas juntas: repouso a 1 g, estável, mas ALTO (longe
// da referência de solo) não é pouso. É o caso de um foguete pendurado numa árvore
// — parado e estável, mas a dezenas de metros do chão.
void test_landing_requires_all_four_conditions(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);
    estimator.begin(PhaseRestore{});

    uint32_t t = 0;
    for (int i = 0; i < 50; ++i) {
        estimator.update(sample_at(kSeaLevelPa, kOneG), t);
        t += 40;
    }
    for (int i = 0; i < 6; ++i) {
        estimator.update(sample_at(kSeaLevelPa, 3000), t);
        t += 40;
    }
    // Preso a ~100 m: 1 g, estável, tempo de sobra — mas NÃO perto do solo.
    const float hung_pa = kSeaLevelPa - 1200.0f;  // ~+100 m
    PhaseState state;
    for (int i = 0; i < 1000; ++i) {  // 40 s
        state = estimator.update(sample_at(hung_pa, kOneG), t);
        t += 40;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(state.phase));
}

// Um snapshot salvo de um voo em andamento: referência de solo congelada, fase de
// voo, com um instante de liftoff.
PhaseSnapshot flight_snapshot(float reference_pa) {
    PhaseSnapshot snap;
    snap.phase         = FlightPhase::Flight;
    snap.reference_pa  = reference_pa;
    snap.liftoff_ms    = 5000;
    snap.has_reference = true;
    return snap;
}

// Um brown-out em pleno voo reseta a placa. Na volta, com o reset NÃO sendo
// power-on limpo e a fase salva sendo de voo, a referência salva é reusada e a
// altitude reportada fica contínua — não recomeça do solo no meio da subida.
void test_reset_in_flight_reuses_the_saved_reference(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);

    // Referência de solo salva; a placa está agora a ~1500 m (pressão menor).
    const float ground_pa = kSeaLevelPa;
    PhaseRestore restore;
    restore.have_saved    = true;
    restore.saved         = flight_snapshot(ground_pa);
    restore.clean_poweron = false;  // brown-out, não power-on
    estimator.begin(restore);

    const float at_altitude_pa = pressure_for_altitude(1500.0f);
    const PhaseState state = estimator.update(sample_at(at_altitude_pa, 0), 6000);

    // Já entra em voo, sem esperar liftoff, e a altitude é a de verdade (~1500 m),
    // medida contra a referência reusada — não ~0.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(state.phase));
    TEST_ASSERT_TRUE(state.has_liftoff);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 1500.0f, state.altitude_m);
    TEST_ASSERT_FALSE(state.altitude_from_gps);
}

// Um power-on limpo NÃO reusa: um brown-out na rampa que voltasse com referência
// velha faria o foguete "voar" com um zero errado. Sem o teste do motivo do reset,
// a referência salva contaminaria um voo novo.
void test_clean_power_on_does_not_reuse(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);

    PhaseRestore restore;
    restore.have_saved    = true;
    restore.saved         = flight_snapshot(kSeaLevelPa);
    restore.clean_poweron = true;  // power-on limpo
    estimator.begin(restore);

    const PhaseState state = estimator.update(sample_at(kSeaLevelPa, kOneG), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(state.phase));
    TEST_ASSERT_FALSE(state.has_liftoff);
}

// Sanidade H4.4: se a referência reusada implicar uma altitude absurda — porque o
// snapshot ficou de um voo em outro local, por exemplo — cai para a altitude do
// GPS e SINALIZA no pacote, para o solo saber que a altitude mudou de fonte.
void test_absurd_reused_reference_falls_back_to_gps(void) {
    PhaseConfig config;
    PhaseEstimator estimator(config);

    // Referência salva absurda: uma pressão que, contra a atual, implica dezenas de
    // milhares de metros.
    PhaseRestore restore;
    restore.have_saved    = true;
    restore.saved         = flight_snapshot(300000.0f);  // ~3 atm: implica ~8 km
    restore.clean_poweron = false;
    estimator.begin(restore);

    SensorSample sample = sample_at(kSeaLevelPa, 0);
    sample.gps.valid      = true;
    sample.gps.altitude_m = 1234.0f;
    const PhaseState state = estimator.update(sample, 6000);

    TEST_ASSERT_TRUE(state.altitude_from_gps);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 1234.0f, state.altitude_m);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_on_the_pad_phase_is_rail_and_altitude_is_zero);
    RUN_TEST(test_pad_reference_is_a_slow_average);
    RUN_TEST(test_liftoff_needs_sustained_acceleration);
    RUN_TEST(test_frozen_reference_is_the_value_from_a_second_before);
    RUN_TEST(test_full_flight_profile_stays_in_flight_through_apogee);
    RUN_TEST(test_landed_is_one_way);
    RUN_TEST(test_landing_requires_all_four_conditions);
    RUN_TEST(test_reset_in_flight_reuses_the_saved_reference);
    RUN_TEST(test_clean_power_on_does_not_reuse);
    RUN_TEST(test_absurd_reused_reference_falls_back_to_gps);
    return UNITY_END();
}
