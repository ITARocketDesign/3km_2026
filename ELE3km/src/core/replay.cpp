#include "core/replay.h"

#include <stdint.h>

#include "core/log_codec.h"

namespace core {
namespace {

// Distância entre dois inteiros de 1e7, sem estourar o i32 (a subtração vai a 64
// bits), saturando de volta em i32.
int32_t abs_diff_1e7(int32_t a, int32_t b) {
    int64_t d = static_cast<int64_t>(a) - static_cast<int64_t>(b);
    if (d < 0) d = -d;
    return d > INT32_MAX ? INT32_MAX : static_cast<int32_t>(d);
}

// Igualdade dos campos BRUTOS que a reconstrução lê. Os dois campos-lacuna não
// entram: são constantes na reconstrução, então não dizem nada sobre o codec.
bool raw_fields_equal(const SensorSample& a, const SensorSample& b) {
    bool accel_ok = true;
    bool gyro_ok = true;
    for (int i = 0; i < 3; ++i) {
        accel_ok = accel_ok && a.accel_mg[i] == b.accel_mg[i];
        gyro_ok = gyro_ok && a.gyro_ddps[i] == b.gyro_ddps[i];
    }
    return a.baro_valid == b.baro_valid && a.pressure_pa == b.pressure_pa &&
           a.temperature_c == b.temperature_c && a.gps.receiving == b.gps.receiving &&
           a.gps.valid == b.gps.valid && a.gps.latitude_1e7 == b.gps.latitude_1e7 &&
           a.gps.longitude_1e7 == b.gps.longitude_1e7 &&
           a.gps.fix_quality == b.gps.fix_quality && a.gps.satellites == b.gps.satellites &&
           a.gps.hdop_half == b.gps.hdop_half && a.imu_valid == b.imu_valid &&
           a.gps_uart_overflows == b.gps_uart_overflows && accel_ok && gyro_ok;
}

}  // namespace

SensorSample sensor_sample_from_record(const LogRecord& record) {
    SensorSample s;
    s.baro_valid    = record.baro_valid;
    s.pressure_pa   = record.pressure_pa;
    s.temperature_c = record.temperature_c;

    s.gps.receiving     = record.gps.receiving;
    s.gps.valid         = record.gps.valid;
    s.gps.latitude_1e7  = record.gps.latitude_1e7;
    s.gps.longitude_1e7 = record.gps.longitude_1e7;
    s.gps.fix_quality   = record.gps.fix_quality;
    s.gps.satellites    = record.gps.satellites;
    s.gps.hdop_half     = record.gps.hdop_half;
    // gps.altitude_m: lacuna conhecida — o registro guarda a altitude DERIVADA do
    // barômetro, não a bruta do GPS. Fica zerada (ver replay.h).
    s.gps.altitude_m = 0.0f;

    for (int i = 0; i < 3; ++i) {
        s.accel_mg[i]  = record.accel_mg[i];
        s.gyro_ddps[i] = record.gyro_ddps[i];
    }

    // imu_valid é recuperável do bit de saúde kImu, que o FlightComputer põe a
    // partir de sample.imu_valid.
    s.imu_valid = (record.health & health_bit::kImu) != 0;

    // accel_saturated: lacuna conhecida — vem do ADC bruto, perdido na conversão
    // para mg. Fica falso (ver replay.h).
    s.accel_saturated = false;

    s.gps_uart_overflows = record.gps_uart_overflows;
    return s;
}

bool record_reconstruction_fields_present() {
    // Um registro com um valor distinto em cada campo bruto que a reconstrução lê.
    // Os valores foram escolhidos para sobreviver à quantização do codec (a
    // temperatura em centésimos, os demais exatos), senão a volta reprovaria por
    // arredondamento e não por campo faltando.
    LogRecord original;
    original.t_ms               = 999;
    original.pressure_pa        = 87654.0f;
    original.temperature_c      = -3.25f;   // −325 centi, exato
    original.baro_valid         = true;
    original.gps.receiving      = true;
    original.gps.valid          = true;
    original.gps.latitude_1e7   = -232012345;
    original.gps.longitude_1e7  = -458765432;
    original.gps.fix_quality    = 1;
    original.gps.satellites     = 8;
    original.gps.hdop_half      = 4;
    original.accel_mg[0]        = 100;
    original.accel_mg[1]        = -200;
    original.accel_mg[2]        = 900;
    original.gyro_ddps[0]       = 12;
    original.gyro_ddps[1]       = -34;
    original.gyro_ddps[2]       = 56;
    original.gps_uart_overflows = 5;
    original.health             = health_bit::kImu;

    const uint16_t boot = 4242;
    uint8_t bytes[kLogRecordSize] = {0};
    if (encode_record(original, boot, bytes, sizeof(bytes)) != kLogRecordSize) {
        return false;
    }
    LogRecord scanned;
    if (scan_records(bytes, sizeof(bytes), boot, &scanned, 1) != 1) {
        return false;
    }

    // A reconstrução a partir do registro original e a partir do que voltou do codec
    // têm que dar a mesma amostra bruta: se um campo bruto saiu do formato, a versão
    // que passou pelo codec o perde e as duas divergem.
    return raw_fields_equal(sensor_sample_from_record(original),
                            sensor_sample_from_record(scanned));
}

ReplayDivergence replay_and_compare(const LogRecord* records, size_t count,
                                    const FlightComputerConfig& config,
                                    const ReplayTolerance& tolerance) {
    ReplayDivergence d;
    if (records == nullptr) {
        return d;
    }

    FlightComputer computer(config);
    for (size_t i = 0; i < count; ++i) {
        const SensorSample sample = sensor_sample_from_record(records[i]);
        const UpdateResult result = computer.update(sample, records[i].t_ms);
        ++d.records;

        // Altitude: a derivada, que é a que o registro grava e o pacote transmite.
        float alt_diff = result.log.altitude_m - records[i].altitude_m;
        if (alt_diff < 0.0f) alt_diff = -alt_diff;
        if (alt_diff > d.max_altitude_diff_m) d.max_altitude_diff_m = alt_diff;
        if (alt_diff > tolerance.altitude_m) d.within_tolerance = false;

        // Posição fundida: o pior dos dois eixos.
        const int32_t lat_diff =
            abs_diff_1e7(result.log.fused_latitude_1e7, records[i].fused_latitude_1e7);
        const int32_t lon_diff =
            abs_diff_1e7(result.log.fused_longitude_1e7, records[i].fused_longitude_1e7);
        const int32_t pos_diff = lat_diff > lon_diff ? lat_diff : lon_diff;
        if (pos_diff > d.max_position_diff_1e7) d.max_position_diff_1e7 = pos_diff;
        if (pos_diff > tolerance.position_1e7) d.within_tolerance = false;

        // Fase e fonte são enums: divergência é sempre falha.
        if (result.log.phase != records[i].phase) {
            ++d.phase_mismatches;
            d.within_tolerance = false;
        }
        if (result.log.position_source != records[i].position_source) {
            ++d.source_mismatches;
            d.within_tolerance = false;
        }
    }
    return d;
}

}  // namespace core
