// Suíte nativa do estimador de dois canais (issue 07). Roda no notebook:
// `pio test -e native`.
//
// Nenhum teste inspeciona a matriz de covariância. O peso do barômetro, o
// crescimento da incerteza num vão de GPS, a rejeição de saturação e a detecção de
// divergência são todos verificados pelo COMPORTAMENTO DA SAÍDA — a estimativa, a
// velocidade, a incerteza reportada e o contador de resets —, nunca pela matriz.
#include <unity.h>

#include <cmath>
#include <limits>

#include "core/estimator.h"

using namespace core;

namespace {

constexpr uint32_t kDtMs = 10;  // 100 Hz

// Ruído determinístico e reproduzível: um LCG barato mapeado para [-amp, +amp].
struct Noise {
    uint32_t state = 0x1234567u;
    float next(float amplitude) {
        state = state * 1664525u + 1013904223u;
        const float unit = static_cast<float>(state >> 8) / 16777216.0f;  // [0,1)
        return (unit * 2.0f - 1.0f) * amplitude;
    }
};

// Uma entrada só com o canal vertical, medição barométrica dada.
EstimatorInput baro_only(float altitude_m) {
    EstimatorInput in;
    in.baro_present = true;
    in.have_baro = true;
    in.baro_altitude_m = altitude_m;
    return in;
}

// Erro horizontal em metros entre uma saída e uma posição verdadeira, na latitude
// do campo de lançamento.
float horizontal_error_m(const EstimatorOutput& out, int32_t truth_lat_1e7,
                         int32_t truth_lon_1e7, float lat_deg) {
    const float mpd_lat = 111320.0f;
    const float mpd_lon = 111320.0f * std::cos(lat_deg * 3.14159265f / 180.0f);
    const float dn = static_cast<float>(out.latitude_1e7 - truth_lat_1e7) / 1e7f * mpd_lat;
    const float de = static_cast<float>(out.longitude_1e7 - truth_lon_1e7) / 1e7f * mpd_lon;
    return std::sqrt(dn * dn + de * de);
}

constexpr int32_t kPadLat = -232012345;  // campo de lançamento
constexpr int32_t kPadLon = -458765432;
constexpr float   kPadLatDeg = -23.2012345f;

}  // namespace

// Uma trajetória sintética com ruído: subida a velocidade constante, medições
// barométricas ruidosas. A estimativa acompanha a verdade dentro de erro limitado,
// e a velocidade vertical converge para a real.
void test_tracks_a_noisy_trajectory_within_bounded_error(void) {
    Estimator estimator;
    Noise noise;

    const float climb_ms = 20.0f;  // abaixo do regime transônico: o barômetro pesa
    uint32_t t = 0;
    EstimatorOutput out;
    for (int i = 0; i < 500; ++i) {  // 5 s
        const float truth = climb_ms * (static_cast<float>(t) / 1000.0f);
        EstimatorInput in = baro_only(truth + noise.next(3.0f));
        in.imu_valid = true;               // velocidade constante ⇒ aceleração 0
        in.vertical_accel_ms2 = 0.0f;
        out = estimator.update(in, t);
        t += kDtMs;
    }

    const float truth_end = climb_ms * (static_cast<float>(t - kDtMs) / 1000.0f);
    TEST_ASSERT_FLOAT_WITHIN(8.0f, truth_end, out.altitude_m);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, climb_ms, out.vertical_speed_ms);
    TEST_ASSERT_EQUAL_UINT8(0, out.resets);  // nenhuma divergência num voo limpo
}

// A correção do GPS reduz o erro de posição. Sem GPS, o canal horizontal fica
// parado na origem enquanto a verdade se afasta; um fix próximo da verdade puxa a
// estimativa de volta, e o erro depois da correção é menor que antes.
void test_gps_correction_reduces_the_error(void) {
    Estimator estimator;

    // Fix inicial: ancora a origem no campo de lançamento.
    EstimatorInput fix;
    fix.have_gps = true;
    fix.gps_lat_1e7 = kPadLat;
    fix.gps_lon_1e7 = kPadLon;
    estimator.update(fix, 0);

    // A verdade se desloca ~40 m para leste; sem GPS a estimativa não sabe disso.
    const int32_t truth_lon = kPadLon + 3910;  // ~40 m em longitude nesta latitude
    uint32_t t = kDtMs;
    EstimatorOutput out;
    for (int i = 0; i < 200; ++i) {  // 2 s de vão
        out = estimator.update(EstimatorInput{}, t);
        t += kDtMs;
    }
    const float error_before = horizontal_error_m(out, kPadLat, truth_lon, kPadLatDeg);

    // Chega um fix na posição verdadeira.
    EstimatorInput fix2;
    fix2.have_gps = true;
    fix2.gps_lat_1e7 = kPadLat;
    fix2.gps_lon_1e7 = truth_lon;
    out = estimator.update(fix2, t);
    const float error_after = horizontal_error_m(out, kPadLat, truth_lon, kPadLatDeg);

    TEST_ASSERT_TRUE(error_before > 20.0f);          // o vão realmente abriu erro
    TEST_ASSERT_TRUE(error_after < error_before);    // a correção reduziu
    TEST_ASSERT_TRUE(error_after < 0.5f * error_before);
}

// Num vão de GPS o filtro roda só em predição, a fonte cai para inercial, e a
// incerteza de posição reportada cresce. A incerteza é saída pública (a precisão
// que um receptor reporta), não a matriz — é por ela que se observa a propagação.
void test_position_uncertainty_grows_during_a_gps_gap(void) {
    Estimator estimator;

    EstimatorInput fix;
    fix.have_gps = true;
    fix.gps_lat_1e7 = kPadLat;
    fix.gps_lon_1e7 = kPadLon;
    const EstimatorOutput anchored = estimator.update(fix, 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(anchored.source));

    uint32_t t = kDtMs;
    EstimatorOutput out;
    for (int i = 0; i < 300; ++i) {  // 3 s de vão
        out = estimator.update(EstimatorInput{}, t);
        t += kDtMs;
    }

    // Só em predição: a fonte é inercial, e a incerteza cresceu bem acima da que
    // ficou logo depois do fix.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Ins),
                            static_cast<uint8_t>(out.source));
    TEST_ASSERT_TRUE(out.horizontal_uncertainty_m > anchored.horizontal_uncertainty_m + 10.0f);
}

// O R do barômetro cai com a velocidade estimada. Com o barômetro TRAVADO em zero e
// uma aceleração vertical constante, em baixa velocidade a estimativa fica presa no
// barômetro (perto de zero); em alta velocidade o barômetro perde peso e a
// estimativa segue a integração inercial, subindo — mesma leitura barométrica, saída
// oposta, verificado só pela saída.
void test_barometer_weight_drops_at_high_speed(void) {
    // Alta velocidade: aceleração grande, o barômetro travado é ignorado no fim.
    {
        Estimator estimator;
        estimator.update(baro_only(0.0f), 0);  // ancora em zero
        uint32_t t = kDtMs;
        EstimatorOutput out;
        for (int i = 0; i < 400; ++i) {  // 4 s ⇒ v ≈ 160 m/s
            EstimatorInput in = baro_only(0.0f);  // barômetro TRAVADO em zero
            in.imu_valid = true;
            in.vertical_accel_ms2 = 40.0f;
            out = estimator.update(in, t);
            t += kDtMs;
        }
        // Apesar do barômetro insistir em zero, a estimativa subiu muito: o peso do
        // barômetro colapsou em alta velocidade.
        TEST_ASSERT_TRUE(out.altitude_m > 50.0f);
    }

    // Baixa velocidade: aceleração minúscula, o barômetro travado domina.
    {
        Estimator estimator;
        estimator.update(baro_only(0.0f), 0);
        uint32_t t = kDtMs;
        EstimatorOutput out;
        for (int i = 0; i < 400; ++i) {  // 4 s ⇒ v ≈ 2 m/s
            EstimatorInput in = baro_only(0.0f);
            in.imu_valid = true;
            in.vertical_accel_ms2 = 0.5f;
            out = estimator.update(in, t);
            t += kDtMs;
        }
        // O barômetro pesa muito: a estimativa fica presa perto de zero.
        TEST_ASSERT_TRUE(std::fabs(out.altitude_m) < 10.0f);
    }
}

// Amostras de IMU no fundo de escala são marcadas e NÃO integradas como válidas: a
// aceleração clipada é excluída da predição. Com a mesma aceleração absurda, o canal
// não-saturado integra e sobe, o saturado quase não se move.
void test_saturated_imu_is_not_integrated(void) {
    // Não-saturado: a aceleração enorme é integrada, a altitude dispara.
    float unsaturated_alt = 0.0f;
    {
        Estimator estimator;
        estimator.update(baro_only(0.0f), 0);  // inicializa o canal em zero
        uint32_t t = kDtMs;
        EstimatorOutput out;
        for (int i = 0; i < 50; ++i) {  // 0,5 s, sem novas medições de barômetro
            EstimatorInput in;
            in.baro_present = true;
            in.imu_valid = true;
            in.vertical_accel_ms2 = 200.0f;  // valor clipado, absurdo
            out = estimator.update(in, t);
            t += kDtMs;
        }
        unsaturated_alt = out.altitude_m;
    }

    // Saturado: a MESMA aceleração é excluída da predição, a altitude quase não anda.
    float saturated_alt = 0.0f;
    {
        Estimator estimator;
        estimator.update(baro_only(0.0f), 0);
        uint32_t t = kDtMs;
        EstimatorOutput out;
        for (int i = 0; i < 50; ++i) {
            EstimatorInput in;
            in.baro_present = true;
            in.imu_valid = true;
            in.imu_saturated = true;         // marcada como saturada
            in.vertical_accel_ms2 = 200.0f;  // deve ser ignorada
            out = estimator.update(in, t);
            t += kDtMs;
        }
        saturated_alt = out.altitude_m;
    }

    TEST_ASSERT_TRUE(unsaturated_alt > 10.0f);        // integrou de fato
    TEST_ASSERT_TRUE(std::fabs(saturated_alt) < 2.0f); // não integrou a saturação
    TEST_ASSERT_TRUE(saturated_alt < 0.1f * unsaturated_alt);
}

// Alimentado com NaN, degraus impossíveis e ruído extremo, a divergência é
// detectada, o estimador reseta para a última medição válida, o contador sobe, e o
// filtro reconverge em poucos ciclos quando as medições boas voltam.
void test_diverges_resets_and_reconverges_on_garbage(void) {
    Estimator estimator;
    Noise noise;

    const float truth = 100.0f;
    uint32_t t = 0;

    // 1 s de barômetro bom: o canal assenta em ~100 m, e 100 vira a última válida.
    for (int i = 0; i < 100; ++i) {
        estimator.update(baro_only(truth), t);
        t += kDtMs;
    }
    TEST_ASSERT_EQUAL_UINT8(0, estimator.resets());

    // Lixo por ~0,5 s: NaN, degraus impossíveis, e ruído extremo dentro da janela.
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < 50; ++i) {
        float z;
        switch (i % 3) {
            case 0: z = nan_v; break;                       // NaN
            case 1: z = truth + 10000.0f; break;            // degrau impossível
            default: z = truth + noise.next(15.0f); break;  // ruído extremo
        }
        estimator.update(baro_only(z), t);
        t += kDtMs;
    }
    // A divergência foi detectada e resetada ao menos uma vez.
    TEST_ASSERT_TRUE(estimator.resets() > 0);

    // Barômetro bom de volta: reconverge para a verdade em poucos ciclos.
    EstimatorOutput out;
    for (int i = 0; i < 30; ++i) {  // 0,3 s
        out = estimator.update(baro_only(truth), t);
        t += kDtMs;
    }
    TEST_ASSERT_TRUE(std::isfinite(out.altitude_m));
    TEST_ASSERT_FLOAT_WITHIN(10.0f, truth, out.altitude_m);
}

// Barômetro ausente: a altitude cai para a do GPS e isso é sinalizado. É o caminho
// que a máquina de saúde da issue 06 e o pacote usam para dizer ao solo que a
// altitude mudou de fonte.
void test_absent_barometer_falls_back_to_gps_altitude(void) {
    Estimator estimator;

    EstimatorInput in;
    in.baro_present = false;   // barômetro morto/ausente no boot
    in.have_gps = true;
    in.gps_altitude_m = 512.0f;
    in.gps_lat_1e7 = kPadLat;
    in.gps_lon_1e7 = kPadLon;

    EstimatorOutput out = estimator.update(in, 0);
    out = estimator.update(in, kDtMs);

    TEST_ASSERT_TRUE(out.altitude_from_gps);
    TEST_ASSERT_TRUE(out.has_altitude);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 512.0f, out.altitude_m);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tracks_a_noisy_trajectory_within_bounded_error);
    RUN_TEST(test_gps_correction_reduces_the_error);
    RUN_TEST(test_position_uncertainty_grows_during_a_gps_gap);
    RUN_TEST(test_barometer_weight_drops_at_high_speed);
    RUN_TEST(test_saturated_imu_is_not_integrated);
    RUN_TEST(test_diverges_resets_and_reconverges_on_garbage);
    RUN_TEST(test_absent_barometer_falls_back_to_gps_altitude);
    return UNITY_END();
}
