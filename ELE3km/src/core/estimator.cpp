#include "core/estimator.h"

#include <cmath>

namespace core {
namespace {

// Fator de inflação do ruído de processo quando a IMU saturou: a amostra de boost
// clipada não pode ser integrada como aceleração válida, então em vez de somá-la o
// filtro alarga a incerteza de predição e deixa a próxima correção reancorar.
constexpr float kSaturatedInflation = 100.0f;

// Metros por grau de latitude, aproximadamente constante. Longitude é este valor
// vezes cos(lat), calculado no ancoramento da origem.
constexpr float kMetersPerDegLat = 111320.0f;
constexpr float kDegPi = 3.14159265358979323846f;

}  // namespace

void Estimator::Channel::predict(float dt, float accel, float accel_var) {
    // x' = F x + B a, com F = [[1, dt],[0, 1]] e B = [0.5 dt², dt].
    p += v * dt + 0.5f * accel * dt * dt;
    v += accel * dt;

    // P' = F P Fᵀ + Q. Desenrolado para a 2×2 simétrica.
    const float P00_new = P00 + 2.0f * dt * P01 + dt * dt * P11;
    const float P01_new = P01 + dt * P11;
    const float P11_new = P11;

    // Q = accel_var · G Gᵀ, com G = [0.5 dt², dt]: aceleração como ruído de processo.
    const float dt2 = dt * dt;
    const float q00 = accel_var * 0.25f * dt2 * dt2;
    const float q01 = accel_var * 0.5f * dt2 * dt;
    const float q11 = accel_var * dt2;

    P00 = P00_new + q00;
    P01 = P01_new + q01;
    P11 = P11_new + q11;
}

void Estimator::Channel::update(float z, float R) {
    // H = [1, 0]: mede só a posição.
    const float S = P00 + R;
    const float K0 = P00 / S;
    const float K1 = P01 / S;
    const float y = z - p;

    p += K0 * y;
    v += K1 * y;

    // P = (I − K H) P, com H = [1, 0].
    const float P01_old = P01;
    P00 = (1.0f - K0) * P00;
    P01 = (1.0f - K0) * P01;
    P11 = P11 - K1 * P01_old;
}

void Estimator::Channel::reset(float z, float position_var, float velocity_var) {
    p = z;
    v = 0.0f;
    P00 = position_var;
    P01 = 0.0f;
    P11 = velocity_var;
    initialized = true;
}

bool Estimator::Channel::healthy(float max_position_var, float max_velocity_var) const {
    if (!std::isfinite(p) || !std::isfinite(v)) return false;
    if (!std::isfinite(P00) || !std::isfinite(P11)) return false;
    if (P00 > max_position_var || P11 > max_velocity_var) return false;
    return true;
}

EstimatorOutput Estimator::update(const EstimatorInput& input, uint32_t t_ms) {
    const float dt =
        have_last_t_ ? static_cast<float>(t_ms - last_t_ms_) / 1000.0f : 0.0f;
    last_t_ms_ = t_ms;
    have_last_t_ = true;

    // ── Canal vertical: predição ────────────────────────────────────────────
    float accel = 0.0f;
    float accel_var = config_.vertical_accel_var;
    if (input.imu_valid && !input.imu_saturated) {
        accel = input.vertical_accel_ms2;
    } else if (input.imu_saturated) {
        // Aceleração excluída da predição; a incerteza é inflada no lugar.
        accel_var = config_.vertical_accel_var * kSaturatedInflation;
    }
    if (vertical_.initialized && dt > 0.0f) {
        vertical_.predict(dt, accel, accel_var);
        if (!vertical_.healthy(config_.max_altitude_var, config_.max_velocity_var)) {
            const float anchor = have_last_baro_    ? last_baro_altitude_m_
                                 : have_last_gps_alt_ ? last_gps_altitude_m_
                                                      : vertical_.p;
            vertical_.reset(anchor, config_.reset_position_var, config_.reset_velocity_var);
            ++resets_;
        }
    }

    // ── Canal vertical: correção ────────────────────────────────────────────
    bool altitude_from_gps = false;
    if (input.baro_present) {
        // Correção barométrica, com R função da velocidade estimada (H12). Um
        // degrau FINITO impossível é rejeitado por taxa e não alimenta o filtro. Uma
        // medição NaN passa por este teste (comparação com NaN é falsa) de
        // propósito: ela alimenta o update, contamina o estado, e é a sanidade
        // pós-passo lá embaixo que a detecta e reseta — é assim que o contador de
        // resets sobe. Rejeitar o NaN aqui esconderia a divergência que a issue pede
        // para o filtro provar que sobrevive.
        if (input.have_baro) {
            const float z = input.baro_altitude_m;
            bool impossible_step = false;
            if (have_prev_baro_ && dt > 0.0f && std::isfinite(z)) {
                const float dt_baro = static_cast<float>(t_ms - prev_baro_ms_) / 1000.0f;
                if (dt_baro > 0.0f) {
                    const float rate = std::fabs(z - prev_baro_altitude_m_) / dt_baro;
                    if (rate > config_.max_alt_rate_ms) impossible_step = true;
                }
            }
            if (!impossible_step) {
                const float speed = std::fabs(vertical_.v);
                float R;
                if (speed <= config_.baro_speed_low_ms) {
                    R = config_.baro_r_low;
                } else if (speed >= config_.baro_speed_high_ms) {
                    R = config_.baro_r_high;
                } else {
                    const float f = (speed - config_.baro_speed_low_ms) /
                                    (config_.baro_speed_high_ms - config_.baro_speed_low_ms);
                    R = config_.baro_r_low + f * (config_.baro_r_high - config_.baro_r_low);
                }
                if (!vertical_.initialized) {
                    vertical_.reset(z, config_.reset_position_var, config_.reset_velocity_var);
                } else {
                    vertical_.update(z, R);
                }
                // Só uma medição FINITA vira "última válida" e âncora de reset — o
                // NaN que acabou de entrar não pode virar o valor para onde o reset
                // aponta.
                if (std::isfinite(z)) {
                    have_last_baro_ = true;
                    last_baro_altitude_m_ = z;
                    prev_baro_altitude_m_ = z;
                    prev_baro_ms_ = t_ms;
                    have_prev_baro_ = true;
                }
            }
        }
        // A altitude do GPS NÃO entra como correção enquanto o barômetro está
        // presente: ela é MSL e a altitude barométrica é relativa à referência de
        // solo — dois zeros diferentes. Fundi-las cruas puxaria a estimativa para um
        // ponto entre os dois referenciais. A altitude do GPS só assume quando o
        // barômetro está ausente (logo abaixo), e aí o bit kAltRef sinaliza ao solo
        // que a altitude mudou de referencial. A reconciliação de referenciais
        // (subtrair a altitude de GPS do solo) é máquina para a fonte de posição
        // da issue 08.
    } else {
        // Barômetro AUSENTE: a altitude cai para a do GPS, e isso é sinalizado.
        altitude_from_gps = true;
        if (input.have_gps && std::isfinite(input.gps_altitude_m)) {
            have_last_gps_alt_ = true;
            last_gps_altitude_m_ = input.gps_altitude_m;
            if (!vertical_.initialized) {
                vertical_.reset(input.gps_altitude_m, config_.reset_position_var,
                                config_.reset_velocity_var);
            } else {
                vertical_.update(input.gps_altitude_m, config_.gps_alt_r);
            }
        }
    }
    if (vertical_.initialized &&
        !vertical_.healthy(config_.max_altitude_var, config_.max_velocity_var)) {
        const float anchor = have_last_baro_    ? last_baro_altitude_m_
                             : have_last_gps_alt_ ? last_gps_altitude_m_
                                                  : vertical_.p;
        vertical_.reset(anchor, config_.reset_position_var, config_.reset_velocity_var);
        ++resets_;
    }

    // ── Canal horizontal: predição em velocidade constante ──────────────────
    // Sem heading, a aceleração de corpo não vira aceleração de navegação: os dois
    // eixos correm em predição de velocidade constante, com ruído de processo alto.
    if (north_.initialized && dt > 0.0f) {
        north_.predict(dt, 0.0f, config_.horizontal_accel_var);
        east_.predict(dt, 0.0f, config_.horizontal_accel_var);
        bool diverged = false;
        if (!north_.healthy(config_.max_horizontal_var, config_.max_velocity_var)) {
            north_.reset(north_.p, config_.reset_position_var, config_.reset_velocity_var);
            diverged = true;
        }
        if (!east_.healthy(config_.max_horizontal_var, config_.max_velocity_var)) {
            east_.reset(east_.p, config_.reset_position_var, config_.reset_velocity_var);
            diverged = true;
        }
        if (diverged) ++resets_;
    }

    // ── Canal horizontal: correção pelo GPS ─────────────────────────────────
    bool corrected_now = false;
    if (input.have_gps) {
        if (!have_origin_) {
            have_origin_ = true;
            origin_lat_1e7 = input.gps_lat_1e7;
            origin_lon_1e7 = input.gps_lon_1e7;
            const float lat_rad =
                static_cast<float>(input.gps_lat_1e7) / 1e7f * kDegPi / 180.0f;
            meters_per_deg_lat_ = kMetersPerDegLat;
            meters_per_deg_lon_ = kMetersPerDegLat * std::cos(lat_rad);
        }
        const float north_m = static_cast<float>(input.gps_lat_1e7 - origin_lat_1e7) /
                              1e7f * meters_per_deg_lat_;
        const float east_m = static_cast<float>(input.gps_lon_1e7 - origin_lon_1e7) /
                             1e7f * meters_per_deg_lon_;

        // Rejeição de salto impossível: a taxa entre este fix e o ANTERIOR aceito,
        // no intervalo real entre eles. Um fix 40 m adiante depois de 2 s de vão é
        // 20 m/s, plausível; medi-lo contra a estimativa parada num ciclo de 10 ms
        // daria 4000 m/s e rejeitaria o fix legítimo.
        bool impossible_jump = false;
        if (have_prev_gps_ && std::isfinite(north_m) && std::isfinite(east_m)) {
            const float dt_gps = static_cast<float>(t_ms - prev_gps_ms_) / 1000.0f;
            if (dt_gps > 0.0f) {
                const float dn = north_m - prev_gps_north_m_;
                const float de = east_m - prev_gps_east_m_;
                const float rate = std::sqrt(dn * dn + de * de) / dt_gps;
                if (rate > config_.max_pos_rate_ms) impossible_jump = true;
            }
        }
        if (!impossible_jump && std::isfinite(north_m) && std::isfinite(east_m)) {
            if (!north_.initialized) {
                north_.reset(north_m, config_.reset_position_var, config_.reset_velocity_var);
                east_.reset(east_m, config_.reset_position_var, config_.reset_velocity_var);
            } else {
                north_.update(north_m, config_.gps_pos_r);
                east_.update(east_m, config_.gps_pos_r);
            }
            have_gps_corrected_ = true;
            corrected_now = true;
            have_prev_gps_ = true;
            prev_gps_north_m_ = north_m;
            prev_gps_east_m_ = east_m;
            prev_gps_ms_ = t_ms;
        }
    }
    if (north_.initialized) {
        bool diverged = false;
        if (!north_.healthy(config_.max_horizontal_var, config_.max_velocity_var)) {
            north_.reset(north_.p, config_.reset_position_var, config_.reset_velocity_var);
            diverged = true;
        }
        if (!east_.healthy(config_.max_horizontal_var, config_.max_velocity_var)) {
            east_.reset(east_.p, config_.reset_position_var, config_.reset_velocity_var);
            diverged = true;
        }
        if (diverged) ++resets_;
    }

    // ── Montagem da saída ───────────────────────────────────────────────────
    EstimatorOutput out;
    out.has_altitude = vertical_.initialized;
    out.altitude_m = vertical_.p;
    out.vertical_speed_ms = vertical_.v;
    out.altitude_from_gps = altitude_from_gps;
    out.vertical_uncertainty_m =
        vertical_.P00 > 0.0f ? std::sqrt(vertical_.P00) : 0.0f;

    out.has_horizontal = north_.initialized && have_origin_;
    if (out.has_horizontal) {
        out.latitude_1e7 =
            origin_lat_1e7 +
            static_cast<int32_t>(north_.p / meters_per_deg_lat_ * 1e7f);
        out.longitude_1e7 =
            origin_lon_1e7 +
            static_cast<int32_t>((meters_per_deg_lon_ != 0.0f
                                      ? east_.p / meters_per_deg_lon_
                                      : 0.0f) *
                                 1e7f);
        const float hvar = north_.P00 + east_.P00;
        out.horizontal_uncertainty_m = hvar > 0.0f ? std::sqrt(hvar) : 0.0f;
        // Corrigido agora é GPS; propagando desde o último fix é a ponte inercial.
        out.source = corrected_now ? PositionSource::Gps : PositionSource::Ins;
    } else {
        out.source = PositionSource::None;
    }

    out.resets = resets_;
    return out;
}

}  // namespace core
