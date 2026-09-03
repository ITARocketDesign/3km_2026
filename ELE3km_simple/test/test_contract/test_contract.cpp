// Suíte de compatibilidade de contrato (issue 01).
//
// O ELE3km_simple fala com o 3km913hzReceiver e o replay do ELE3km SEM alteração.
// Isso só vale se os codecs copiados forem byte-a-byte idênticos ao ELE3km. Esta
// suíte é a rede que pega uma cópia corrompida: ela afirma os tamanhos exatos, o
// magic/versão do pacote, e a ida e volta do registro de log com CRC.
//
// Nenhum teste inspeciona estado interno — só o comportamento externo do núcleo.
#include <unity.h>

#include "core/log_codec.h"
#include "core/telemetry_codec.h"

using namespace core;

namespace {

// Um pacote só-altitude plausível.
TelemetryPacket altitude_packet() {
    TelemetryPacket p;
    p.form               = PacketForm::AltitudeOnly;
    p.sequence           = 1234;
    p.t_ds               = 5678;
    p.altitude_m         = 321;
    p.vertical_speed_dms = -45;
    p.phase              = FlightPhase::Flight;
    p.position_source    = PositionSource::None;
    p.fix_quality        = 3;
    p.health             = health_bit::kBaro | health_bit::kSx1276;
    p.satellites         = 7;
    p.hdop_half          = 4;
    return p;
}

// O mesmo, na forma completa, com latitude e longitude.
TelemetryPacket full_packet() {
    TelemetryPacket p     = altitude_packet();
    p.form                = PacketForm::Full;
    p.position_source     = PositionSource::Gps;
    p.latitude_1e7        = -235678901;   // ~ -23,5678901°
    p.longitude_1e7       = -467890123;   // ~ -46,7890123°
    return p;
}

// Um registro de log com campos brutos plausíveis.
LogRecord log_record() {
    LogRecord r;
    r.t_ms          = 123456;
    r.sequence      = 42;
    r.pressure_pa   = 90000.0f;
    r.temperature_c = 21.5f;
    r.altitude_m    = 1050.0f;
    r.baro_valid    = true;
    r.gps.latitude_1e7  = -235678901;
    r.gps.longitude_1e7 = -467890123;
    r.gps.satellites    = 8;
    r.accel_mg[0]   = 12;
    r.accel_mg[1]   = -34;
    r.accel_mg[2]   = 1001;
    r.health        = health_bit::kBaro | health_bit::kSx1276 | health_bit::kSd;
    return r;
}

}  // namespace

// ── Tracer: o pacote só-altitude tem 12 B exatos, com magic 0xE / versão 0x1 ──
void test_altitude_packet_is_12_bytes_with_magic(void) {
    uint8_t out[kMaxPacketSize] = {0};
    const size_t n = encode_packet(altitude_packet(), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(kAltitudePacketSize, n);
    TEST_ASSERT_EQUAL_UINT(12u, n);
    TEST_ASSERT_EQUAL_HEX8((kPacketMagic << 4) | kPacketVersion, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE1, out[0]);
}

// ── O pacote só-altitude sobrevive à ida e volta ────────────────────────────
void test_altitude_packet_roundtrips(void) {
    const TelemetryPacket in = altitude_packet();
    uint8_t buf[kMaxPacketSize] = {0};
    const size_t n = encode_packet(in, buf, sizeof(buf));

    TelemetryPacket out;
    TEST_ASSERT_TRUE(decode_packet(buf, n, out));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(PacketForm::AltitudeOnly),
                           static_cast<int>(out.form));
    TEST_ASSERT_EQUAL_UINT16(in.sequence, out.sequence);
    TEST_ASSERT_EQUAL_UINT16(in.t_ds, out.t_ds);
    TEST_ASSERT_EQUAL_INT16(in.altitude_m, out.altitude_m);
    TEST_ASSERT_EQUAL_INT16(in.vertical_speed_dms, out.vertical_speed_dms);
    TEST_ASSERT_EQUAL_HEX8(in.health, out.health);
}

// ── O pacote completo tem 20 B exatos e sobrevive à ida e volta ─────────────
void test_full_packet_is_20_bytes_and_roundtrips(void) {
    const TelemetryPacket in = full_packet();
    uint8_t buf[kMaxPacketSize] = {0};
    const size_t n = encode_packet(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(kFullPacketSize, n);
    TEST_ASSERT_EQUAL_UINT(20u, n);
    TEST_ASSERT_EQUAL_HEX8(0xE1, buf[0]);

    TelemetryPacket out;
    TEST_ASSERT_TRUE(decode_packet(buf, n, out));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(PacketForm::Full),
                           static_cast<int>(out.form));
    TEST_ASSERT_EQUAL_INT32(in.latitude_1e7, out.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(in.longitude_1e7, out.longitude_1e7);
    TEST_ASSERT_EQUAL_INT16(in.altitude_m, out.altitude_m);
}

// ── O registro de log tem 64 B exatos e o CRC fecha na ida e volta ──────────
void test_log_record_is_64_bytes_and_crc_roundtrips(void) {
    const uint16_t boot = 7;
    uint8_t buf[kLogRecordSize] = {0};
    const size_t n = encode_record(log_record(), boot, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(kLogRecordSize, n);
    TEST_ASSERT_EQUAL_UINT(64u, n);
    TEST_ASSERT_EQUAL_HEX8(kLogRecordMagic, buf[0]);

    LogRecord out;
    uint16_t out_boot = 0;
    TEST_ASSERT_TRUE(decode_record(buf, n, out, out_boot));
    TEST_ASSERT_EQUAL_UINT16(boot, out_boot);
    TEST_ASSERT_EQUAL_UINT32(42u, out.sequence);
    TEST_ASSERT_EQUAL_INT16(1001, out.accel_mg[2]);
}

// ── O CRC copiado realmente valida: um byte corrompido é rejeitado ─────────
void test_log_record_rejects_corrupted_crc(void) {
    uint8_t buf[kLogRecordSize] = {0};
    encode_record(log_record(), 7, buf, sizeof(buf));
    buf[10] ^= 0xFF;  // corrompe um byte de payload

    LogRecord out;
    uint16_t out_boot = 0;
    TEST_ASSERT_FALSE(decode_record(buf, sizeof(buf), out, out_boot));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_altitude_packet_is_12_bytes_with_magic);
    RUN_TEST(test_altitude_packet_roundtrips);
    RUN_TEST(test_full_packet_is_20_bytes_and_roundtrips);
    RUN_TEST(test_log_record_is_64_bytes_and_crc_roundtrips);
    RUN_TEST(test_log_record_rejects_corrupted_crc);
    return UNITY_END();
}
