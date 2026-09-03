// Suíte nativa do harness de replay (issue 14). Roda no notebook:
// `pio test -e native`.
//
// O harness reconstrói amostras a partir dos registros brutos e realimenta o
// núcleo; os testes exercem só o comportamento externo — a amostra reconstruída, o
// ciclo fechado sintético, e a comparação que reporta divergências.
#include <unity.h>

#include <cmath>

#include "core/log_codec.h"
#include "core/replay.h"

using namespace core;

namespace {

// Pressão de solo para uma altitude dada (inverte a ISA da troposfera).
float pressure_for_altitude(float altitude_m) {
    return 101325.0f * std::pow(1.0f - altitude_m / 44330.0f, 5.255f);
}

// Um registro com campos brutos distintos em cada posição, para que uma troca de
// campo na reconstrução apareça.
LogRecord distinctive_record() {
    LogRecord r;
    r.t_ms               = 12345;
    r.pressure_pa        = 98765.4f;
    r.temperature_c      = -12.5f;
    r.baro_valid         = true;
    r.gps.receiving      = true;
    r.gps.valid          = true;
    r.gps.latitude_1e7   = -232012345;
    r.gps.longitude_1e7  = -458765432;
    r.gps.fix_quality    = 2;
    r.gps.satellites     = 9;
    r.gps.hdop_half      = 3;
    r.accel_mg[0]        = 11;
    r.accel_mg[1]        = -22;
    r.accel_mg[2]        = 1003;
    r.gyro_ddps[0]       = 44;
    r.gyro_ddps[1]       = -55;
    r.gyro_ddps[2]       = 66;
    r.gps_uart_overflows = 7;
    r.health             = health_bit::kImu | health_bit::kBaro;
    return r;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// A reconstrução devolve os campos brutos do registro na amostra, com imu_valid
// recuperado do bit de saúde. Os dois campos que o registro não carrega saem
// zerados — lacunas conhecidas, não descuido.
void test_reconstructed_sample_carries_the_raw_fields(void) {
    const LogRecord r = distinctive_record();

    const SensorSample s = sensor_sample_from_record(r);

    TEST_ASSERT_TRUE(s.baro_valid);
    TEST_ASSERT_EQUAL_FLOAT(98765.4f, s.pressure_pa);
    TEST_ASSERT_EQUAL_FLOAT(-12.5f, s.temperature_c);
    TEST_ASSERT_TRUE(s.gps.receiving);
    TEST_ASSERT_TRUE(s.gps.valid);
    TEST_ASSERT_EQUAL_INT32(-232012345, s.gps.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(-458765432, s.gps.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(2, s.gps.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(9, s.gps.satellites);
    TEST_ASSERT_EQUAL_UINT8(3, s.gps.hdop_half);
    TEST_ASSERT_EQUAL_INT16(11, s.accel_mg[0]);
    TEST_ASSERT_EQUAL_INT16(-22, s.accel_mg[1]);
    TEST_ASSERT_EQUAL_INT16(1003, s.accel_mg[2]);
    TEST_ASSERT_EQUAL_INT16(44, s.gyro_ddps[0]);
    TEST_ASSERT_EQUAL_INT16(-55, s.gyro_ddps[1]);
    TEST_ASSERT_EQUAL_INT16(66, s.gyro_ddps[2]);
    TEST_ASSERT_EQUAL_UINT32(7, s.gps_uart_overflows);

    // imu_valid vem do bit de saúde kImu.
    TEST_ASSERT_TRUE(s.imu_valid);

    // Lacunas conhecidas: o registro de 64 B não as carrega.
    TEST_ASSERT_FALSE(s.accel_saturated);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.gps.altitude_m);
}

// Sem o bit de saúde kImu, imu_valid volta falso: é assim que o registro de um voo
// sem IMU viva se reconstrói honestamente.
void test_reconstructed_imu_valid_follows_the_health_bit(void) {
    LogRecord r = distinctive_record();
    r.health = health_bit::kBaro;  // sem kImu

    const SensorSample s = sensor_sample_from_record(r);

    TEST_ASSERT_FALSE(s.imu_valid);
}

// AC 8: a verificação de suficiência de campos passa — todos os campos brutos que a
// reconstrução usa sobrevivem à volta pelo codec do registro. É o guarda que quebra
// se alguém tirar um campo bruto do formato de 64 B.
void test_reconstruction_fields_are_present_in_the_record(void) {
    TEST_ASSERT_TRUE(record_reconstruction_fields_present());
}

// Gera um voo sintético pela FlightComputer e coleta os registros de log. Fica em
// voo o tempo todo (nunca perto do solo → não pousa), então todo ciclo é gravado e
// a reprodução pode ser exata. Só usa o subconjunto reconstruível: accel_saturated
// sempre falso e gps.altitude_m sempre 0, os dois campos que o registro não carrega.
size_t synthesize_flight(const FlightComputerConfig& config, LogRecord* out, size_t capacity) {
    FlightComputer source(config);
    size_t n = 0;
    uint32_t t = 0;

    auto feed = [&](float altitude_m, int16_t az_mg, bool gps_valid) {
        if (n >= capacity) return;
        SensorSample s;
        s.baro_valid  = true;
        s.pressure_pa = pressure_for_altitude(altitude_m);
        s.accel_mg[2] = az_mg;
        s.imu_valid   = true;  // sem saturação: subconjunto reconstruível
        if (gps_valid) {
            s.gps.receiving     = true;
            s.gps.valid         = true;
            s.gps.latitude_1e7  = -232012345;
            s.gps.longitude_1e7 = -458765432;
            s.gps.fix_quality   = 1;
            s.gps.satellites    = 9;
            s.gps.hdop_half     = 3;
            s.gps.altitude_m    = 0.0f;  // lacuna conhecida: fica 0 dos dois lados
        }
        out[n++] = source.update(s, t).log;
        t += 40;
    };

    for (int i = 0; i < 10; ++i) feed(0.0f, 1000, false);     // rampa
    for (int i = 0; i < 10; ++i) feed(0.0f, 3000, false);     // liftoff + boost
    for (int i = 0; i < 200; ++i) feed(1000.0f, 1000, true);  // voo alto, com GPS
    return n;
}

// AC 6 (o ciclo fecha) + AC 2/3/4: um log sintético gerado pelo próprio núcleo,
// serializado, varrido de volta e reproduzido pelo harness devolve saídas
// IDÊNTICAS — divergência zero sob tolerância exata.
void test_synthetic_flight_replays_identically(void) {
    FlightComputerConfig config;  // o mesmo dos dois lados: gerar e reproduzir

    static LogRecord logs[300];
    const size_t count = synthesize_flight(config, logs, 300);
    TEST_ASSERT_GREATER_THAN_UINT(100, count);

    // Serializa cabeçalho + registros como o cartão faria.
    const uint16_t boot = 4242;
    static uint8_t bytes[kLogHeaderSize + 300 * kLogRecordSize];
    LogHeader header;
    header.boot_count = boot;
    size_t off = encode_header(header, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL_size_t(kLogHeaderSize, off);
    for (size_t i = 0; i < count; ++i) {
        off += encode_record(logs[i], boot, bytes + off, sizeof(bytes) - off);
    }

    // Varre de volta a partir do primeiro registro (depois do bloco de cabeçalho).
    static LogRecord recovered[300];
    const size_t scanned = scan_records(bytes + kLogHeaderSize, count * kLogRecordSize, boot,
                                        recovered, 300);
    TEST_ASSERT_EQUAL_size_t(count, scanned);

    // Reproduz e compara com tolerância exata.
    ReplayTolerance exact;  // tudo zero
    const ReplayDivergence d = replay_and_compare(recovered, scanned, config, exact);

    TEST_ASSERT_EQUAL_UINT32(count, d.records);
    TEST_ASSERT_TRUE(d.within_tolerance);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.max_altitude_diff_m);
    TEST_ASSERT_EQUAL_INT32(0, d.max_position_diff_1e7);
    TEST_ASSERT_EQUAL_UINT32(0, d.phase_mismatches);
    TEST_ASSERT_EQUAL_UINT32(0, d.source_mismatches);
}

// AC 4/5: é assim que uma mudança no estimador é avaliada — o núcleo alterado
// reproduz um log antigo e a comparação reporta a divergência. Aqui o limiar de
// liftoff sobe acima do boost sintético, então o núcleo alterado nunca decola e a
// fase diverge da gravada. A fase é enum: nenhuma tolerância a perdoa.
void test_comparison_reports_divergence_from_a_changed_core(void) {
    FlightComputerConfig generator;
    static LogRecord logs[300];
    const size_t count = synthesize_flight(generator, logs, 300);
    TEST_ASSERT_GREATER_THAN_UINT(100, count);

    FlightComputerConfig changed = generator;
    changed.phase.liftoff_accel_mg = 3500;  // acima do boost de 3000 mg → nunca decola

    // Tolerância generosa em altitude e posição, mas a fase é exata.
    ReplayTolerance loose;
    loose.altitude_m   = 1.0e9f;
    loose.position_1e7 = 2000000000;
    const ReplayDivergence d = replay_and_compare(logs, count, changed, loose);

    TEST_ASSERT_EQUAL_UINT32(count, d.records);
    TEST_ASSERT_FALSE(d.within_tolerance);
    TEST_ASSERT_GREATER_THAN_UINT32(0, d.phase_mismatches);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reconstructed_sample_carries_the_raw_fields);
    RUN_TEST(test_reconstructed_imu_valid_follows_the_health_bit);
    RUN_TEST(test_reconstruction_fields_are_present_in_the_record);
    RUN_TEST(test_synthetic_flight_replays_identically);
    RUN_TEST(test_comparison_reports_divergence_from_a_changed_core);
    return UNITY_END();
}
