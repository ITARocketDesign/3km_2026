// Suíte nativa da máquina de fonte de posição (issue 08). Roda no notebook:
// `pio test -e native`.
//
// A máquina é pura e o tempo é parâmetro. Os testes exercem só o comportamento
// externo: a fonte escolhida, a forma do pacote que a confiança decide, a posição
// transmitida e a idade — nunca estado interno.
#include <unity.h>

#include "core/position_source.h"

using namespace core;

namespace {

constexpr int32_t kLat = -232012345;  // campo de lançamento, hemisfério sul
constexpr int32_t kLon = -458765432;  // hemisfério oeste

// Uma entrada com fix de GPS válido, e a posição fundida ancorada nele.
PositionSourceInput fix_at(int32_t lat, int32_t lon) {
    PositionSourceInput in;
    in.have_gps_fix = true;
    in.gps_lat_1e7 = lat;
    in.gps_lon_1e7 = lon;
    in.has_fused = true;
    in.fused_lat_1e7 = lat;
    in.fused_lon_1e7 = lon;
    return in;
}

// Sem fix nesta amostra, mas com a posição fundida que o estimador propaga.
PositionSourceInput no_fix_fused_at(int32_t lat, int32_t lon) {
    PositionSourceInput in;
    in.have_gps_fix = false;
    in.has_fused = true;
    in.fused_lat_1e7 = lat;
    in.fused_lon_1e7 = lon;
    return in;
}

}  // namespace

// Um fix válido produz fonte GPS, confiança (pacote completo) e a posição fundida.
void test_valid_fix_selects_gps(void) {
    PositionSourceMachine machine;

    const PositionSourceOutput out = machine.update(fix_at(kLat, kLon), 0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.confident);
    TEST_ASSERT_TRUE(out.has_position);
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
}

// Perdido o fix além do limiar de frescor, mas dentro da janela de ponte, a fonte
// vira Ins e a posição transmitida é a FUNDIDA propagada — não o último fix.
void test_stale_within_bridge_selects_ins(void) {
    PositionSourceMachine machine;
    machine.update(fix_at(kLat, kLon), 0);

    // 1 s depois, sem fix, o estimador propagou a posição para um pouco ao norte.
    const int32_t drifted_lat = kLat + 3000;  // ~33 m
    const PositionSourceOutput out =
        machine.update(no_fix_fused_at(drifted_lat, kLon), 1000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Ins),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.confident);
    TEST_ASSERT_TRUE(out.has_position);
    TEST_ASSERT_EQUAL_INT32(drifted_lat, out.latitude_1e7);
}

// Passada a janela de ponte, a fonte vira LastValid: transmite a ÚLTIMA posição
// de GPS válida — não a fundida, que já derivou —, com a idade correta.
void test_beyond_bridge_selects_last_valid_with_age(void) {
    PositionSourceMachine machine;
    machine.update(fix_at(kLat, kLon), 0);

    // 20 s depois (> janela de 15 s), a fundida derivou longe; a máquina ignora.
    const int32_t drifted_lat = kLat + 900000;  // ~10 km de deriva inercial
    const PositionSourceOutput out =
        machine.update(no_fix_fused_at(drifted_lat, kLon), 20000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.confident);
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);  // último fix, não a deriva
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT32(20000, out.fix_age_ms);
}

// Reaquirido o GPS depois de um vão longo, a fonte volta a Gps e a idade zera.
void test_gps_reacquired_returns_to_gps(void) {
    PositionSourceMachine machine;
    machine.update(fix_at(kLat, kLon), 0);
    machine.update(no_fix_fused_at(kLat + 900000, kLon), 20000);  // LastValid

    const int32_t new_lat = kLat + 5000;
    const PositionSourceOutput out = machine.update(fix_at(new_lat, kLon), 25000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_EQUAL_UINT32(0, out.fix_age_ms);
    TEST_ASSERT_EQUAL_INT32(new_lat, out.latitude_1e7);
}

// IMU ausente desabilita a ponte: um fix obsoleto que com IMU seria Ins cai direto
// para LastValid, sem tentar propagação inercial. A idade continua correta.
void test_absent_imu_skips_ins_falls_to_last_valid(void) {
    PositionSourceMachine machine;

    PositionSourceInput first = fix_at(kLat, kLon);
    first.imu_present = false;
    machine.update(first, 0);

    // 1 s depois: com IMU seria Ins (dentro da janela). Sem IMU, LastValid.
    PositionSourceInput stale = no_fix_fused_at(kLat + 3000, kLon);
    stale.imu_present = false;
    const PositionSourceOutput out = machine.update(stale, 1000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);  // último fix, não a fundida
    TEST_ASSERT_EQUAL_UINT32(1000, out.fix_age_ms);
}

// Antes de qualquer fix, a fonte é None e não há confiança: o pacote será
// só-altitude, e nenhuma posição é transmitida.
void test_no_fix_ever_is_none_and_not_confident(void) {
    PositionSourceMachine machine;

    PositionSourceInput in;  // sem fix, sem fundida
    const PositionSourceOutput out = machine.update(in, 0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::None),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_FALSE(out.confident);
    TEST_ASSERT_FALSE(out.has_position);
}

// A janela de ponte é configurável: com uma janela curta, um vão que seria Ins sob
// o default vira LastValid mais cedo.
void test_bridge_window_is_configurable(void) {
    PositionSourceConfig config;
    config.bridge_window_ms = 5000;
    PositionSourceMachine machine(config);
    machine.update(fix_at(kLat, kLon), 0);

    // 8 s: além da janela curta (5 s), mas dentro do default de 15 s.
    const PositionSourceOutput out =
        machine.update(no_fix_fused_at(kLat + 3000, kLon), 8000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(out.source));
}

// ── Pós-pouso (issue 13) ─────────────────────────────────────────────────────
//
// Em POUSADO a máquina troca de regime: em vez de escolher entre GPS fresco, ponte
// inercial e última válida, ela FILTRA os fixes (satélites ≥ 4 e HDOP ≤ 5,0) e
// TRANSMITE A MÉDIA dos aceitos. O campo de amostras diz ao operador se ele caminha
// para um ponto ou para um círculo.

// Um fix com sat e HDOP dentro do critério, marcado como amostra de POUSADO.
PositionSourceInput landed_fix(int32_t lat, int32_t lon, uint8_t sats, uint8_t hdop_half) {
    PositionSourceInput in;
    in.is_landed = true;
    in.have_gps_fix = true;
    in.gps_lat_1e7 = lat;
    in.gps_lon_1e7 = lon;
    in.satellites = sats;
    in.hdop_half = hdop_half;
    return in;
}

// Um único fix aceito em POUSADO já dá posição confiável de fonte GPS, a própria
// posição do fix, e o campo de amostras em 1.
void test_landed_single_accepted_fix_is_the_position(void) {
    PositionSourceMachine machine;

    const PositionSourceOutput out = machine.update(landed_fix(kLat, kLon, 8, 4), 0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.confident);
    TEST_ASSERT_TRUE(out.has_position);
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(1, out.samples);
}

// O filtro é dureza: um fix com menos de 4 satélites OU HDOP pior que 5,0 é
// rejeitado, e a posição transmitida NUNCA o inclui. Um aceito fixa a média; os
// rejeitados que vêm depois, longe, não a movem nem sobem o campo de amostras.
void test_landed_rejects_low_sat_or_high_hdop(void) {
    PositionSourceMachine machine;

    // Um bom fix fixa a média.
    machine.update(landed_fix(kLat, kLon, 8, 4), 0);
    // Poucos satélites: rejeitado, a 10 km de distância.
    machine.update(landed_fix(kLat + 900000, kLon, 3, 4), 1000);
    // HDOP 6,0 (× 2 = 12 > 10): rejeitado, também longe.
    const PositionSourceOutput out =
        machine.update(landed_fix(kLon, kLat + 900000, 9, 12), 2000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);   // ainda o único aceito
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(1, out.samples);           // os rejeitados não contam
}

// A posição transmitida é a MÉDIA dos fixes aceitos, e o campo de amostras
// acompanha a contagem: 1, depois 2, depois 3. É a transição contínua da issue —
// sem flag binário "pronta/não pronta".
void test_landed_averages_accepted_fixes_and_counts_samples(void) {
    PositionSourceMachine machine;

    // Três fixes bons espalhados 60 m em torno de um centro.
    const int32_t c_lat = kLat;
    const int32_t c_lon = kLon;

    PositionSourceOutput out = machine.update(landed_fix(c_lat - 6000, c_lon, 7, 5), 0);
    TEST_ASSERT_EQUAL_UINT8(1, out.samples);
    TEST_ASSERT_EQUAL_INT32(c_lat - 6000, out.latitude_1e7);

    out = machine.update(landed_fix(c_lat + 6000, c_lon, 7, 5), 1000);
    TEST_ASSERT_EQUAL_UINT8(2, out.samples);
    TEST_ASSERT_EQUAL_INT32(c_lat, out.latitude_1e7);  // média dos dois: o centro

    out = machine.update(landed_fix(c_lat, c_lon + 3000, 7, 5), 2000);
    TEST_ASSERT_EQUAL_UINT8(3, out.samples);
    TEST_ASSERT_EQUAL_INT32(c_lat, out.latitude_1e7);        // (−6000+6000+0)/3
    TEST_ASSERT_EQUAL_INT32(c_lon + 1000, out.longitude_1e7);  // (0+0+3000)/3
}

// Passados 3 fixes a média é considerada confiável e se estabiliza; o campo de
// amostras satura em 7 para caber nos 3 bits do byte 17 — "7 ou mais", não some.
void test_landed_average_is_stable_and_samples_saturate(void) {
    PositionSourceMachine machine;

    PositionSourceOutput out;
    for (int i = 0; i < 20; ++i) {
        out = machine.update(landed_fix(kLat, kLon, 6, 6), static_cast<uint32_t>(i) * 1000);
    }

    TEST_ASSERT_EQUAL_UINT8(7, out.samples);          // saturado, não 20 nem 0
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);  // média de fixes iguais é estável
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
}

// Zero fixes aceitos em POUSADO: a máquina transmite a última posição de GPS
// VÁLIDA DE VOO, como fonte LastValid, com o campo de amostras em 0. Um fix ruim
// no chão (poucos satélites) não conta e não sobrescreve a posição de voo.
void test_landed_zero_accepted_fixes_uses_last_flight_position(void) {
    PositionSourceMachine machine;

    // Em voo, um bom fix é registrado como a última posição válida de voo.
    machine.update(fix_at(kLat, kLon), 0);

    // Pousa e só chegam fixes ruins (3 satélites), num lugar completamente diferente.
    const PositionSourceOutput out =
        machine.update(landed_fix(kLat + 900000, kLon + 900000, 3, 4), 30000);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.confident);
    TEST_ASSERT_EQUAL_INT32(kLat, out.latitude_1e7);   // a de voo, não o fix ruim
    TEST_ASSERT_EQUAL_INT32(kLon, out.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(0, out.samples);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_landed_single_accepted_fix_is_the_position);
    RUN_TEST(test_landed_rejects_low_sat_or_high_hdop);
    RUN_TEST(test_landed_averages_accepted_fixes_and_counts_samples);
    RUN_TEST(test_landed_average_is_stable_and_samples_saturate);
    RUN_TEST(test_landed_zero_accepted_fixes_uses_last_flight_position);
    RUN_TEST(test_valid_fix_selects_gps);
    RUN_TEST(test_stale_within_bridge_selects_ins);
    RUN_TEST(test_beyond_bridge_selects_last_valid_with_age);
    RUN_TEST(test_gps_reacquired_returns_to_gps);
    RUN_TEST(test_absent_imu_skips_ins_falls_to_last_valid);
    RUN_TEST(test_no_fix_ever_is_none_and_not_confident);
    RUN_TEST(test_bridge_window_is_configurable);
    return UNITY_END();
}
