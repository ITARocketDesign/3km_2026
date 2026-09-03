#include "core/telemetry_codec.h"

namespace core {
namespace {

// Escritas byte a byte: o layout é little-endian por definição, não por
// coincidência com a endianness do host que compilou.
void put_u16(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

uint16_t get_u16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0]) | static_cast<uint16_t>(in[1] << 8);
}

void put_i16(uint8_t* out, int16_t v) {
    put_u16(out, static_cast<uint16_t>(v));
}

int16_t get_i16(const uint8_t* in) {
    return static_cast<int16_t>(get_u16(in));
}

void put_i32(uint8_t* out, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    out[0] = static_cast<uint8_t>(u & 0xFF);
    out[1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((u >> 24) & 0xFF);
}

int32_t get_i32(const uint8_t* in) {
    const uint32_t u = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                       (static_cast<uint32_t>(in[2]) << 16) |
                       (static_cast<uint32_t>(in[3]) << 24);
    return static_cast<int32_t>(u);
}

uint8_t encode_flags(const TelemetryPacket& p) {
    return static_cast<uint8_t>((static_cast<uint8_t>(p.phase) & 0x07) |
                                ((static_cast<uint8_t>(p.position_source) & 0x03) << 3) |
                                ((p.fix_quality & 0x07) << 5));
}

uint8_t encode_gps_byte(const TelemetryPacket& p) {
    const uint8_t sats = p.satellites > 15 ? 15 : p.satellites;
    const uint8_t hdop = p.hdop_half > 15 ? 15 : p.hdop_half;
    return static_cast<uint8_t>((sats & 0x0F) | (hdop << 4));
}

size_t packet_size(PacketForm form) {
    return form == PacketForm::Full ? kFullPacketSize : kAltitudePacketSize;
}

}  // namespace

// As duas formas diferem por um único bloco de 8 B — latitude e longitude — no
// meio do layout. Um offset móvel escreve as duas sem duplicar o resto, que é
// exatamente onde uma divergência entre elas passaria despercebida.
size_t encode_packet(const TelemetryPacket& packet, uint8_t* out, size_t capacity) {
    const size_t size = packet_size(packet.form);
    if (out == nullptr || capacity < size) {
        return 0;
    }

    size_t i = 0;
    out[i++] = static_cast<uint8_t>((kPacketMagic << 4) | (kPacketVersion & 0x0F));
    put_u16(&out[i], packet.sequence);
    i += 2;
    put_u16(&out[i], packet.t_ds);
    i += 2;
    if (packet.form == PacketForm::Full) {
        put_i32(&out[i], packet.latitude_1e7);
        i += 4;
        put_i32(&out[i], packet.longitude_1e7);
        i += 4;
    }
    put_i16(&out[i], packet.altitude_m);
    i += 2;
    put_i16(&out[i], packet.vertical_speed_dms);
    i += 2;
    out[i++] = encode_flags(packet);
    out[i++] = packet.health;
    out[i++] = encode_gps_byte(packet);

    return i;
}

bool decode_packet(const uint8_t* in, size_t len, TelemetryPacket& out) {
    if (in == nullptr || (len != kAltitudePacketSize && len != kFullPacketSize)) {
        return false;
    }
    if (((in[0] >> 4) & 0x0F) != kPacketMagic || (in[0] & 0x0F) != kPacketVersion) {
        return false;
    }

    const bool full = (len == kFullPacketSize);
    out.form = full ? PacketForm::Full : PacketForm::AltitudeOnly;

    size_t i = 1;
    out.sequence = get_u16(&in[i]);
    i += 2;
    out.t_ds = get_u16(&in[i]);
    i += 2;
    if (full) {
        out.latitude_1e7 = get_i32(&in[i]);
        i += 4;
        out.longitude_1e7 = get_i32(&in[i]);
        i += 4;
    } else {
        // A forma só-altitude não carrega posição, e não deve deixar a posição
        // anterior parada no destino fingindo que carregou.
        out.latitude_1e7  = 0;
        out.longitude_1e7 = 0;
    }
    out.altitude_m = get_i16(&in[i]);
    i += 2;
    out.vertical_speed_dms = get_i16(&in[i]);
    i += 2;
    out.phase           = static_cast<FlightPhase>(in[i] & 0x07);
    out.position_source = static_cast<PositionSource>((in[i] >> 3) & 0x03);
    out.fix_quality     = static_cast<uint8_t>((in[i] >> 5) & 0x07);
    ++i;
    out.health = in[i++];
    out.satellites = static_cast<uint8_t>(in[i] & 0x0F);
    out.hdop_half  = static_cast<uint8_t>((in[i] >> 4) & 0x0F);

    return true;
}

}  // namespace core
