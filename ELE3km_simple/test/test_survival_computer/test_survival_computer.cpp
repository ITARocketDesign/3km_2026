// Suíte do SurvivalComputer (issue 03) — o orquestrador enxuto do fallback.
//
// Entra pela interface pública: monta { LogRecord, TelemetryPacket } a partir de
// uma amostra e do tempo. Nenhum teste inspeciona estado interno — só o que a
// função devolve e o que o codec congelado faz com isso.
#include <cmath>

#include <unity.h>

#include "core/altitude.h"
#include "core/log_codec.h"
#include "core/survival_computer.h"
#include "core/telemetry_codec.h"
#include "core/types.h"

using namespace core;

namespace {

// Uma amostra de barômetro válido a uma pressão conhecida.
SensorSample baro_sample(float pressure_pa) {
    SensorSample s;
    s.baro_valid  = true;
    s.pressure_pa = pressure_pa;
    return s;
}

// Uma coordenada conhecida (~ -23,5°, -46,6°) para exercitar a ida e volta do
// pacote completo. Negativa nos dois eixos para pegar o sinal do i32.
constexpr int32_t kLat = -235000000;
constexpr int32_t kLon = -466000000;

// Baro válido MAIS um fix de GPS de qualidade dada. `hdop_half` é HDOP × 2, então
// hdop_half = 10 é HDOP 5,0. `receiving` é "o receptor está falando", separado do
// fix — a porta de fix é só satélites/HDOP.
SensorSample gps_sample(uint8_t satellites, uint8_t hdop_half, bool receiving = true) {
    SensorSample s     = baro_sample(90000.0f);
    s.gps.receiving    = receiving;
    s.gps.satellites   = satellites;
    s.gps.hdop_half    = hdop_half;
    s.gps.fix_quality  = 1;
    s.gps.latitude_1e7  = kLat;
    s.gps.longitude_1e7 = kLon;
    return s;
}

}  // namespace

// ── Tracer: um pacote só-altitude sai, com a pressão-altitude do datum fixo ──
void test_tracer_emits_altitude_only_packet_with_barometric_altitude(void) {
    SurvivalComputer c;
    const Outputs o = c.update(baro_sample(90000.0f), 0);

    TEST_ASSERT_TRUE(o.has_packet);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::AltitudeOnly),
                          static_cast<int>(o.packet.form));

    // Codifica em exatamente 12 B (contrato congelado do receptor).
    uint8_t buf[kMaxPacketSize] = {0};
    TEST_ASSERT_EQUAL_UINT(kAltitudePacketSize, encode_packet(o.packet, buf, sizeof(buf)));

    // A altitude é a pressão-altitude contra o datum ISA fixo.
    const int16_t expected =
        static_cast<int16_t>(std::lround(altitude_from_pressure(90000.0f, kFixedDatumPa)));
    TEST_ASSERT_EQUAL_INT16(expected, o.packet.altitude_m);

    // Campos fixados nesta fatia.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FlightPhase::Flight),
                          static_cast<int>(o.packet.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PositionSource::None),
                          static_cast<int>(o.packet.position_source));
}

// ── Um registro de log é montado em TODO ciclo, com sequência por ciclo ─────
void test_builds_a_log_record_every_cycle(void) {
    SurvivalComputer c;
    const Outputs o0 = c.update(baro_sample(90000.0f), 0);
    const Outputs o1 = c.update(baro_sample(90000.0f), 20);

    // t_ms da aquisição e sequência do registro (por ciclo, começando em 0).
    TEST_ASSERT_EQUAL_UINT32(0u, o0.record.t_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, o0.record.sequence);
    TEST_ASSERT_EQUAL_UINT32(20u, o1.record.t_ms);
    TEST_ASSERT_EQUAL_UINT32(1u, o1.record.sequence);

    // Bruto ecoado + altitude derivada + fase/fonte fixas nesta fatia.
    TEST_ASSERT_EQUAL_FLOAT(90000.0f, o0.record.pressure_pa);
    TEST_ASSERT_TRUE(o0.record.baro_valid);
    const float expected = altitude_from_pressure(90000.0f, kFixedDatumPa);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, expected, o0.record.altitude_m);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FlightPhase::Flight),
                          static_cast<int>(o0.record.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PositionSource::None),
                          static_cast<int>(o0.record.position_source));
}

// ── Altitude barométrica acende o bit 6 de saúde (kAltRef) ──────────────────
void test_barometric_altitude_sets_health_bit6(void) {
    SurvivalComputer c;
    const Outputs o = c.update(baro_sample(90000.0f), 0);
    TEST_ASSERT_TRUE((o.packet.health & health_bit::kAltRef) != 0);
    TEST_ASSERT_TRUE((o.record.health & health_bit::kAltRef) != 0);
}

// ── Baro ausente: altitude 0 e bit 6 apagado (fallback de GPS é a issue 05) ─
void test_absent_baro_yields_zero_altitude_and_clears_bit6(void) {
    SurvivalComputer c;
    SensorSample no_baro;  // baro_valid = false por default
    const Outputs o = c.update(no_baro, 0);

    TEST_ASSERT_TRUE(o.has_packet);
    TEST_ASSERT_EQUAL_INT16(0, o.packet.altitude_m);
    TEST_ASSERT_FALSE((o.packet.health & health_bit::kAltRef) != 0);
}

// ── A altitude é mantida nos subciclos sem leitura fresca de baro ───────────
void test_altitude_is_held_across_baroless_subcycles(void) {
    SurvivalComputer c;
    c.update(baro_sample(90000.0f), 0);  // estabelece a altitude no ciclo 0

    SensorSample no_baro;
    Outputs b;
    for (uint32_t t = 20; t <= 1000; t += 20) {
        b = c.update(no_baro, t);  // baro sem leitura fresca até o próximo pacote
    }

    TEST_ASSERT_TRUE(b.has_packet);  // pacote em t = 1000
    const int16_t expected =
        static_cast<int16_t>(std::lround(altitude_from_pressure(90000.0f, kFixedDatumPa)));
    TEST_ASSERT_EQUAL_INT16(expected, b.packet.altitude_m);           // mantida, não 0
    TEST_ASSERT_TRUE((b.packet.health & health_bit::kAltRef) != 0);   // ainda barométrica
}

// ── Sequência incrementa por pacote; tempo em ds desde o boot ───────────────
void test_packet_sequence_increments_and_time_is_ds_since_boot(void) {
    SurvivalComputer c;
    const Outputs a = c.update(baro_sample(90000.0f), 0);  // pacote 0, t_ds 0

    Outputs b;
    for (uint32_t t = 20; t <= 1000; t += 20) {
        b = c.update(baro_sample(90000.0f), t);            // pacote 1 em t = 1000
    }

    TEST_ASSERT_TRUE(a.has_packet);
    TEST_ASSERT_TRUE(b.has_packet);
    TEST_ASSERT_EQUAL_UINT16(0u, a.packet.t_ds);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(a.packet.sequence + 1), b.packet.sequence);
    TEST_ASSERT_EQUAL_UINT16(10u, b.packet.t_ds);          // 1000 ms = 10 ds
}

// ── A telemetria sai a ~1 Hz, não a cada volta de 50 Hz ─────────────────────
void test_transmits_at_about_one_hertz(void) {
    SurvivalComputer c;
    int packets = 0;
    for (uint32_t t = 0; t <= 2000; t += 20) {  // 2 s de laço a 50 Hz
        if (c.update(baro_sample(90000.0f), t).has_packet) {
            ++packets;
        }
    }
    TEST_ASSERT_EQUAL_INT(3, packets);  // t = 0, 1000, 2000
}

// ── IMU bruto (issue 04): aceleração e giro copiados para o registro ────────
void test_raw_imu_is_copied_to_the_record(void) {
    SurvivalComputer c;
    SensorSample s = baro_sample(90000.0f);
    s.imu_valid    = true;
    s.accel_mg[0]  = 100;
    s.accel_mg[1]  = -250;
    s.accel_mg[2]  = 1000;
    s.gyro_ddps[0] = -30;
    s.gyro_ddps[1] = 45;
    s.gyro_ddps[2] = 12;

    const Outputs o = c.update(s, 0);

    TEST_ASSERT_EQUAL_INT16(100, o.record.accel_mg[0]);
    TEST_ASSERT_EQUAL_INT16(-250, o.record.accel_mg[1]);
    TEST_ASSERT_EQUAL_INT16(1000, o.record.accel_mg[2]);
    TEST_ASSERT_EQUAL_INT16(-30, o.record.gyro_ddps[0]);
    TEST_ASSERT_EQUAL_INT16(45, o.record.gyro_ddps[1]);
    TEST_ASSERT_EQUAL_INT16(12, o.record.gyro_ddps[2]);

    // Sem saturação, o bit 7 de saúde fica apagado.
    TEST_ASSERT_FALSE((o.record.health & health_bit::kAccelSat) != 0);
}

// ── Saturação de IMU acende o bit 7 SÓ no registro, nunca no pacote ─────────
void test_accel_saturation_sets_record_health_bit7_only(void) {
    SurvivalComputer c;
    SensorSample s     = baro_sample(90000.0f);
    s.imu_valid        = true;
    s.accel_saturated  = true;

    const Outputs o = c.update(s, 0);

    // Propaga para o registro (byte de saúde 54, bit 7).
    TEST_ASSERT_TRUE((o.record.health & health_bit::kAccelSat) != 0);
    // Decisão record-only: o pacote de rádio congelado não carrega a saturação.
    TEST_ASSERT_TRUE(o.has_packet);
    TEST_ASSERT_FALSE((o.packet.health & health_bit::kAccelSat) != 0);
    // E round-trip pelo log_codec congelado preserva o bit.
    uint8_t buf[kLogRecordSize] = {0};
    TEST_ASSERT_EQUAL_UINT(kLogRecordSize, encode_record(o.record, 0, buf, sizeof(buf)));
    LogRecord back;
    uint16_t  boot = 0;
    TEST_ASSERT_TRUE(decode_record(buf, sizeof(buf), back, boot));
    TEST_ASSERT_TRUE((back.health & health_bit::kAccelSat) != 0);
}

// ── Velocidade vertical = diferença finita da pressão-altitude, dm/s ────────
void test_vertical_speed_is_finite_difference_of_pressure_altitude(void) {
    SurvivalComputer c;

    // Primeira leitura de baro: sem anterior contra quem diferir → velocidade 0.
    const Outputs o0 = c.update(baro_sample(90000.0f), 0);
    TEST_ASSERT_EQUAL_INT16(0, o0.record.vertical_speed_dms);

    // 100 ms depois, a uma pressão MENOR: subiu, velocidade positiva.
    const Outputs o1 = c.update(baro_sample(89000.0f), 100);

    const float a0 = altitude_from_pressure(90000.0f, kFixedDatumPa);
    const float a1 = altitude_from_pressure(89000.0f, kFixedDatumPa);
    const int16_t expected =
        static_cast<int16_t>(std::lround((a1 - a0) * 10000.0f / 100.0f));
    TEST_ASSERT_TRUE(expected > 0);
    TEST_ASSERT_INT16_WITHIN(1, expected, o1.record.vertical_speed_dms);
}

// ── A velocidade vertical entra no pacote (e no registro) na cadência de TX ─
void test_vertical_speed_present_in_packet(void) {
    SurvivalComputer c;
    c.update(baro_sample(90000.0f), 0);  // datum de baro em t = 0 (pacote #0)

    SensorSample no_baro;
    for (uint32_t t = 20; t < 1000; t += 20) {
        c.update(no_baro, t);  // segura entre leituras de baro
    }
    const Outputs o = c.update(baro_sample(89000.0f), 1000);  // baro fresco, pacote #1

    TEST_ASSERT_TRUE(o.has_packet);
    const float a0 = altitude_from_pressure(90000.0f, kFixedDatumPa);
    const float a1 = altitude_from_pressure(89000.0f, kFixedDatumPa);
    const int16_t expected =
        static_cast<int16_t>(std::lround((a1 - a0) * 10000.0f / 1000.0f));
    TEST_ASSERT_INT16_WITHIN(1, expected, o.record.vertical_speed_dms);
    TEST_ASSERT_INT16_WITHIN(1, expected, o.packet.vertical_speed_dms);
}

// ── Um salto absurdo de altitude satura o i16 sem estourar ──────────────────
void test_vertical_speed_saturates_without_overflow(void) {
    SurvivalComputer c;
    c.update(baro_sample(100000.0f), 0);
    // ~5 km de queda de pressão-altitude em 20 ms → muito além de 3276,7 m/s.
    const Outputs o = c.update(baro_sample(50000.0f), 20);
    TEST_ASSERT_EQUAL_INT16(32767, o.record.vertical_speed_dms);
}

// ── Fix válido → pacote completo de 20 B com posição e fonte = GPS ──────────
void test_valid_fix_emits_full_packet_with_position(void) {
    SurvivalComputer c;
    const Outputs o = c.update(gps_sample(6, 4), 0);  // 6 sats, HDOP 2,0 → fix bom

    TEST_ASSERT_TRUE(o.has_packet);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::Full),
                          static_cast<int>(o.packet.form));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PositionSource::Gps),
                          static_cast<int>(o.packet.position_source));

    // Codifica em exatamente 20 B e a ida e volta preserva lat/lon brutos.
    uint8_t buf[kMaxPacketSize] = {0};
    TEST_ASSERT_EQUAL_UINT(kFullPacketSize, encode_packet(o.packet, buf, sizeof(buf)));
    TelemetryPacket back;
    TEST_ASSERT_TRUE(decode_packet(buf, kFullPacketSize, back));
    TEST_ASSERT_EQUAL_INT32(kLat, back.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLon, back.longitude_1e7);
}

// Forma do pacote de um único ciclo com o fix dado.
PacketForm form_for(uint8_t satellites, uint8_t hdop_half) {
    SurvivalComputer c;
    return c.update(gps_sample(satellites, hdop_half), 0).packet.form;
}

// ── Porta de fix nos limites: sats 3/4 e HDOP 5,0/5,5 (hdop_half 10/11) ─────
// hdop_half é HDOP × 2 (passos de 0,5), então 5,0 e 5,1 não são distinguíveis no
// campo que o núcleo enxerga; o degrau representável acima de 5,0 é 5,5.
void test_fix_gate_at_boundaries(void) {
    // Satélites: 3 reprova, 4 aprova (HDOP folgado nos dois).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::AltitudeOnly),
                          static_cast<int>(form_for(3, 2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::Full),
                          static_cast<int>(form_for(4, 2)));
    // HDOP: 5,0 (hdop_half 10) aprova, 5,5 (hdop_half 11) reprova (sats folgados).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::Full),
                          static_cast<int>(form_for(6, 10)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::AltitudeOnly),
                          static_cast<int>(form_for(6, 11)));
}

// ── Satélites/HDOP/qualidade seguem no pacote nas DUAS formas ───────────────
void test_gps_quality_fields_present_in_both_forms(void) {
    // Com fix → pacote completo.
    {
        SurvivalComputer c;
        const Outputs o = c.update(gps_sample(7, 6), 0);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::Full),
                              static_cast<int>(o.packet.form));
        TEST_ASSERT_EQUAL_UINT8(7, o.packet.satellites);
        TEST_ASSERT_EQUAL_UINT8(6, o.packet.hdop_half);
        TEST_ASSERT_EQUAL_UINT8(1, o.packet.fix_quality);
    }
    // Sem fix → só-altitude, mas satélites/HDOP/qualidade continuam preenchidos.
    {
        SurvivalComputer c;
        const Outputs o = c.update(gps_sample(2, 6), 0);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::AltitudeOnly),
                              static_cast<int>(o.packet.form));
        TEST_ASSERT_EQUAL_UINT8(2, o.packet.satellites);
        TEST_ASSERT_EQUAL_UINT8(6, o.packet.hdop_half);
        TEST_ASSERT_EQUAL_UINT8(1, o.packet.fix_quality);
    }
}

// ── Bit 2 de saúde (GPS) segue "recebendo", não "tem fix" ───────────────────
void test_health_gps_bit_follows_receiving_not_fix(void) {
    // Recebendo mas sem fix (2 sats): bit GPS ligado, mesmo caindo para só-altitude.
    {
        SurvivalComputer c;
        const Outputs o = c.update(gps_sample(2, 4, /*receiving=*/true), 0);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PacketForm::AltitudeOnly),
                              static_cast<int>(o.packet.form));
        TEST_ASSERT_TRUE((o.packet.health & health_bit::kGps) != 0);
        TEST_ASSERT_TRUE((o.record.health & health_bit::kGps) != 0);
    }
    // Mudo: bit GPS apagado (independe de haver satélites reportados).
    {
        SurvivalComputer c;
        const Outputs o = c.update(gps_sample(6, 4, /*receiving=*/false), 0);
        TEST_ASSERT_FALSE((o.packet.health & health_bit::kGps) != 0);
        TEST_ASSERT_FALSE((o.record.health & health_bit::kGps) != 0);
    }
}

// ── Registro: posição fundida = bruta do GPS (não há fusão nesta barra) ─────
void test_record_fused_position_equals_raw_gps(void) {
    SurvivalComputer c;
    const Outputs o = c.update(gps_sample(6, 4), 0);  // fix válido

    // Bruto e "fundido" carregam a mesma posição; a fonte do registro segue o fix.
    TEST_ASSERT_EQUAL_INT32(kLat, o.record.gps.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLat, o.record.fused_latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLon, o.record.fused_longitude_1e7);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PositionSource::Gps),
                          static_cast<int>(o.record.position_source));

    // Ida e volta pelo log_codec congelado preserva bruto e fundido (offsets 24/28
    // e 32/36) sem mudar layout nem versão.
    uint8_t buf[kLogRecordSize] = {0};
    TEST_ASSERT_EQUAL_UINT(kLogRecordSize, encode_record(o.record, 0, buf, sizeof(buf)));
    LogRecord back;
    uint16_t  boot = 0;
    TEST_ASSERT_TRUE(decode_record(buf, sizeof(buf), back, boot));
    TEST_ASSERT_EQUAL_INT32(kLat, back.gps.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLat, back.fused_latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(kLon, back.fused_longitude_1e7);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tracer_emits_altitude_only_packet_with_barometric_altitude);
    RUN_TEST(test_builds_a_log_record_every_cycle);
    RUN_TEST(test_barometric_altitude_sets_health_bit6);
    RUN_TEST(test_absent_baro_yields_zero_altitude_and_clears_bit6);
    RUN_TEST(test_altitude_is_held_across_baroless_subcycles);
    RUN_TEST(test_packet_sequence_increments_and_time_is_ds_since_boot);
    RUN_TEST(test_transmits_at_about_one_hertz);
    RUN_TEST(test_raw_imu_is_copied_to_the_record);
    RUN_TEST(test_accel_saturation_sets_record_health_bit7_only);
    RUN_TEST(test_vertical_speed_is_finite_difference_of_pressure_altitude);
    RUN_TEST(test_vertical_speed_present_in_packet);
    RUN_TEST(test_vertical_speed_saturates_without_overflow);
    RUN_TEST(test_valid_fix_emits_full_packet_with_position);
    RUN_TEST(test_fix_gate_at_boundaries);
    RUN_TEST(test_gps_quality_fields_present_in_both_forms);
    RUN_TEST(test_health_gps_bit_follows_receiving_not_fix);
    RUN_TEST(test_record_fused_position_equals_raw_gps);
    return UNITY_END();
}
