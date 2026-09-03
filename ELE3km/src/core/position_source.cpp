#include "core/position_source.h"

namespace core {

PositionSourceOutput PositionSourceMachine::update(const PositionSourceInput& input,
                                                   uint32_t t_ms) {
    // POUSADO (issue 13): outro regime. Filtra os fixes por satélites e HDOP e
    // transmite a MÉDIA dos aceitos. Os fixes de pós-pouso NÃO tocam o último fix
    // de voo: o fallback de zero amostras é a última posição VÁLIDA DE VOO, e um
    // fix ruim sob copa não pode sobrescrevê-la.
    if (input.is_landed) {
        const bool accepted = input.have_gps_fix &&
                              input.satellites >= config_.landed_min_satellites &&
                              input.hdop_half <= config_.landed_max_hdop_half;
        if (accepted) {
            landed_sum_lat_1e7_ += input.gps_lat_1e7;
            landed_sum_lon_1e7_ += input.gps_lon_1e7;
            ++landed_count_;
        }

        PositionSourceOutput out;
        if (landed_count_ > 0) {
            const int64_t n = static_cast<int64_t>(landed_count_);
            out.source = PositionSource::Gps;
            out.confident = true;
            out.has_position = true;
            out.latitude_1e7 = static_cast<int32_t>(landed_sum_lat_1e7_ / n);
            out.longitude_1e7 = static_cast<int32_t>(landed_sum_lon_1e7_ / n);
            out.samples = landed_count_ > 7 ? 7 : static_cast<uint8_t>(landed_count_);
        } else if (have_last_fix_) {
            // Zero amostras aceitas: a última posição de GPS válida DE VOO ainda é
            // útil — um fix na descida limita o ponto de pouso a um raio pequeno.
            out.source = PositionSource::LastValid;
            out.confident = true;
            out.has_position = true;
            out.latitude_1e7 = last_lat_1e7_;
            out.longitude_1e7 = last_lon_1e7_;
        }
        // Sem nenhum fix de voo e sem amostra aceita: None, só-altitude (default).
        return out;
    }

    if (input.have_gps_fix) {
        have_last_fix_ = true;
        last_lat_1e7_ = input.gps_lat_1e7;
        last_lon_1e7_ = input.gps_lon_1e7;
        last_fix_ms_ = t_ms;
    }

    PositionSourceOutput out;
    if (!have_last_fix_) {
        return out;  // None, só-altitude
    }

    const uint32_t age = t_ms - last_fix_ms_;
    out.confident = true;
    out.has_position = true;
    out.fix_age_ms = age;

    if (age <= config_.gps_fresh_ms) {
        out.source = PositionSource::Gps;
        out.latitude_1e7 = input.has_fused ? input.fused_lat_1e7 : last_lat_1e7_;
        out.longitude_1e7 = input.has_fused ? input.fused_lon_1e7 : last_lon_1e7_;
    } else if (input.imu_present && age <= config_.bridge_window_ms) {
        // Dentro da janela de ponte e com IMU viva: Ins, posição fundida propagada.
        // Sem IMU a ponte é desabilitada e a perda de GPS cai direto para LastValid.
        out.source = PositionSource::Ins;
        out.latitude_1e7 = input.has_fused ? input.fused_lat_1e7 : last_lat_1e7_;
        out.longitude_1e7 = input.has_fused ? input.fused_lon_1e7 : last_lon_1e7_;
    } else {
        // Passada a janela: LastValid, a última posição de GPS válida com a idade.
        out.source = PositionSource::LastValid;
        out.latitude_1e7 = last_lat_1e7_;
        out.longitude_1e7 = last_lon_1e7_;
    }
    return out;
}

}  // namespace core
