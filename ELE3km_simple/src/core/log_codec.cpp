#include "core/log_codec.h"

#include <math.h>
#include <string.h>

namespace core {
namespace {

// CRC-16/CCITT-FALSE: polinômio 0x1021, inicial 0xFFFF, sem reflexão. Escolhido
// por ser o que qualquer ferramenta de análise reimplementa em cinco linhas — a
// ferramenta de recuperação não é necessariamente este código.
uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

void put_u16(uint8_t* out, size_t offset, uint16_t value) {
    out[offset]     = static_cast<uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t* out, size_t offset, uint32_t value) {
    out[offset]     = static_cast<uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void put_i16(uint8_t* out, size_t offset, int16_t value) {
    put_u16(out, offset, static_cast<uint16_t>(value));
}

void put_i32(uint8_t* out, size_t offset, int32_t value) {
    put_u32(out, offset, static_cast<uint32_t>(value));
}

void put_f32(uint8_t* out, size_t offset, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(out, offset, bits);
}

uint16_t get_u16(const uint8_t* in, size_t offset) {
    return static_cast<uint16_t>(in[offset] | (static_cast<uint16_t>(in[offset + 1]) << 8));
}

uint32_t get_u32(const uint8_t* in, size_t offset) {
    return static_cast<uint32_t>(in[offset]) | (static_cast<uint32_t>(in[offset + 1]) << 8) |
           (static_cast<uint32_t>(in[offset + 2]) << 16) |
           (static_cast<uint32_t>(in[offset + 3]) << 24);
}

int16_t get_i16(const uint8_t* in, size_t offset) {
    return static_cast<int16_t>(get_u16(in, offset));
}

int32_t get_i32(const uint8_t* in, size_t offset) {
    return static_cast<int32_t>(get_u32(in, offset));
}

float get_f32(const uint8_t* in, size_t offset) {
    const uint32_t bits = get_u32(in, offset);
    float          value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// Temperatura em centésimos de grau cabe em i16 de -327 a +327 °C, que cobre
// qualquer coisa que este veículo possa medir sem gastar os 4 B de um float.
int16_t to_centicelsius(float celsius) {
    const float scaled = roundf(celsius * 100.0f);
    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return static_cast<int16_t>(scaled);
}

uint16_t saturate_u16(uint32_t value) {
    return value > 65535u ? static_cast<uint16_t>(65535) : static_cast<uint16_t>(value);
}

namespace record_flag {
constexpr uint8_t kBaroValid = 1 << 5;
constexpr uint8_t kGpsValid = 1 << 6;
constexpr uint8_t kGpsReceiving = 1 << 7;
}  // namespace record_flag

}  // namespace

size_t encode_header(const LogHeader& header, uint8_t* out, size_t capacity) {
    if (out == nullptr || capacity < kLogHeaderSize) {
        return 0;
    }
    memset(out, 0, kLogHeaderSize);

    out[0] = kLogRecordMagic;
    out[1] = header.format_version;
    put_u16(out, 2, header.boot_count);
    put_f32(out, 4, header.reference_pa);
    out[8] = header.pin_map_revision;
    out[9] = header.boot_loop ? 1 : 0;
    out[10] = header.recent_reset_count;
    // O CRC do cabeçalho fica no mesmo lugar relativo que o do registro, para
    // que uma inspeção manual do arquivo não precise de duas regras.
    put_u16(out, kLogRecordSize - 2, crc16(out, kLogRecordSize - 2));

    return kLogHeaderSize;
}

bool decode_header(const uint8_t* in, size_t len, LogHeader& out) {
    if (in == nullptr || len < kLogHeaderSize) {
        return false;
    }
    if (in[0] != kLogRecordMagic || in[1] != kLogFormatVersion) {
        return false;
    }
    if (get_u16(in, kLogRecordSize - 2) != crc16(in, kLogRecordSize - 2)) {
        return false;
    }

    out.format_version   = in[1];
    out.boot_count       = get_u16(in, 2);
    out.reference_pa     = get_f32(in, 4);
    out.pin_map_revision = in[8];
    out.boot_loop        = in[9] != 0;
    out.recent_reset_count = in[10];
    return true;
}

size_t encode_record(const LogRecord& record, uint16_t boot_count, uint8_t* out,
                     size_t capacity) {
    if (out == nullptr || capacity < kLogRecordSize) {
        return 0;
    }
    memset(out, 0, kLogRecordSize);

    out[0] = kLogRecordMagic;
    out[1] = static_cast<uint8_t>(
        (static_cast<uint8_t>(record.phase) & 0x07) |
        ((static_cast<uint8_t>(record.position_source) & 0x03) << 3) |
        (record.baro_valid ? record_flag::kBaroValid : 0) |
        (record.gps.valid ? record_flag::kGpsValid : 0) |
        (record.gps.receiving ? record_flag::kGpsReceiving : 0));
    put_u16(out, 2, boot_count);
    put_u32(out, 4, record.sequence);
    put_u32(out, 8, record.t_ms);
    put_f32(out, 12, record.pressure_pa);
    put_i16(out, 16, to_centicelsius(record.temperature_c));
    put_i16(out, 18, record.vertical_speed_dms);
    put_f32(out, 20, record.altitude_m);
    put_i32(out, 24, record.gps.latitude_1e7);
    put_i32(out, 28, record.gps.longitude_1e7);
    put_i32(out, 32, record.fused_latitude_1e7);
    put_i32(out, 36, record.fused_longitude_1e7);
    for (size_t i = 0; i < 3; ++i) {
        put_i16(out, 40 + i * 2, record.accel_mg[i]);
        put_i16(out, 46 + i * 2, record.gyro_ddps[i]);
    }
    out[52] = static_cast<uint8_t>((record.gps.satellites & 0x0F) |
                                   ((record.gps.hdop_half & 0x0F) << 4));
    out[53] = record.gps.fix_quality;
    out[54] = record.health;
    out[55] = record.estimator_resets;
    put_u16(out, 56, saturate_u16(record.gps_uart_overflows));
    put_u16(out, 58, record.stack_watermark_flight);
    put_u16(out, 60, record.stack_watermark_io);
    put_u16(out, 62, crc16(out, kLogRecordSize - 2));

    return kLogRecordSize;
}

bool decode_record(const uint8_t* in, size_t len, LogRecord& out, uint16_t& boot_count) {
    if (in == nullptr || len < kLogRecordSize) {
        return false;
    }
    if (in[0] != kLogRecordMagic) {
        return false;
    }
    if (get_u16(in, 62) != crc16(in, kLogRecordSize - 2)) {
        return false;
    }

    const uint8_t flags = in[1];
    out.phase           = static_cast<FlightPhase>(flags & 0x07);
    out.position_source = static_cast<PositionSource>((flags >> 3) & 0x03);
    out.baro_valid      = (flags & record_flag::kBaroValid) != 0;
    out.gps.valid       = (flags & record_flag::kGpsValid) != 0;
    out.gps.receiving   = (flags & record_flag::kGpsReceiving) != 0;

    boot_count              = get_u16(in, 2);
    out.sequence            = get_u32(in, 4);
    out.t_ms                = get_u32(in, 8);
    out.pressure_pa         = get_f32(in, 12);
    out.temperature_c       = static_cast<float>(get_i16(in, 16)) / 100.0f;
    out.vertical_speed_dms  = get_i16(in, 18);
    out.altitude_m          = get_f32(in, 20);
    out.gps.latitude_1e7    = get_i32(in, 24);
    out.gps.longitude_1e7   = get_i32(in, 28);
    out.fused_latitude_1e7  = get_i32(in, 32);
    out.fused_longitude_1e7 = get_i32(in, 36);
    for (size_t i = 0; i < 3; ++i) {
        out.accel_mg[i]  = get_i16(in, 40 + i * 2);
        out.gyro_ddps[i] = get_i16(in, 46 + i * 2);
    }
    out.gps.satellites           = static_cast<uint8_t>(in[52] & 0x0F);
    out.gps.hdop_half            = static_cast<uint8_t>((in[52] >> 4) & 0x0F);
    out.gps.fix_quality          = in[53];
    out.health                   = in[54];
    out.estimator_resets         = in[55];
    out.gps_uart_overflows       = get_u16(in, 56);
    out.stack_watermark_flight   = get_u16(in, 58);
    out.stack_watermark_io       = get_u16(in, 60);
    return true;
}

size_t scan_records(const uint8_t* data, size_t len, uint16_t boot_count, LogRecord* out,
                    size_t capacity) {
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    size_t found = 0;
    // O último registro é descartado quando o arquivo foi cortado no meio dele:
    // `offset + kLogRecordSize <= len` é a condição, e é por isso que um corte
    // de energia em pleno voo custa no máximo um registro.
    for (size_t offset = 0; offset + kLogRecordSize <= len && found < capacity;
         offset += kLogRecordSize) {
        LogRecord record;
        uint16_t  record_boot = 0;
        if (!decode_record(data + offset, kLogRecordSize, record, record_boot)) {
            continue;
        }
        // Registro perfeitamente válido, de OUTRO voo: o arquivo pré-alocado
        // caiu nos clusters do voo anterior e aquele dado nunca foi sobrescrito.
        if (record_boot != boot_count) {
            continue;
        }
        out[found++] = record;
    }
    return found;
}

}  // namespace core
