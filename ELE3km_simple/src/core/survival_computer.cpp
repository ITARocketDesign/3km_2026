#include "core/survival_computer.h"

#include <cmath>

#include "core/altitude.h"

namespace core {
namespace {

// Float para i16, saturando no fundo de escala do campo. Serve à altitude (m) e à
// velocidade vertical (dm/s) — os dois campos são i16 no pacote e no registro.
int16_t saturate_i16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return static_cast<int16_t>(std::lround(v));
}

// Decissegundos desde o boot, saturando em ~109 min (contrato do pacote).
uint16_t to_decaseconds(uint32_t t_ms) {
    const uint32_t ds = t_ms / 100u;
    return ds > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(ds);
}

}  // namespace

Outputs SurvivalComputer::update(const SensorSample& sample, uint32_t t_ms,
                                 const IoSubsystemHealth& io) {
    Outputs out;

    // Porta de fix (issue 05): satélites ≥ 4 E HDOP ≤ 5,0. hdop_half é HDOP × 2,
    // então HDOP ≤ 5,0 é hdop_half ≤ 10. Crua, sem staleness nem última-válida —
    // a máquina de fonte de posição (None/Ins/LastValid) é da issue 08. Fix bom →
    // pacote completo com posição bruta; sem fix → só-altitude, sem posição. Não há
    // fusão: bruto e "fundido" carregam a mesma posição.
    const GpsFix& gps          = sample.gps;
    const bool    gps_fix_valid = gps.satellites >= 4 && gps.hdop_half <= 10;

    if (sample.baro_valid && sample.pressure_pa > 0.0f) {
        const float altitude_m = altitude_from_pressure(sample.pressure_pa, kFixedDatumPa);

        // Velocidade vertical: diferença finita contra a última leitura FRESCA de
        // baro, não contra o ciclo anterior (a maioria não traz baro). dm/s =
        // Δm × 10 / Δs = Δm × 10000 / Δms. O primeiro baro não tem anterior → 0.
        if (have_baro_altitude_ && t_ms != last_baro_ms_) {
            const float dt_ms = static_cast<float>(t_ms - last_baro_ms_);
            last_vertical_speed_dms_ =
                saturate_i16((altitude_m - last_altitude_m_) * 10000.0f / dt_ms);
        }

        last_altitude_m_    = altitude_m;
        last_baro_ms_       = t_ms;
        have_baro_altitude_ = true;
    }

    // ── Registro, montado em todo ciclo ─────────────────────────────────────
    // Espelho da amostra crua + a altitude derivada. Os campos das issues 04/07/08
    // (velocidade, posição fundida, IMU, watermarks) ficam em zero até a issue
    // produtora — o layout de 64 B não pode mudar depois.
    LogRecord& r         = out.record;
    r.t_ms               = t_ms;
    r.sequence           = record_sequence_++;
    r.pressure_pa        = sample.pressure_pa;
    r.temperature_c      = sample.temperature_c;
    r.altitude_m         = last_altitude_m_;
    r.vertical_speed_dms = last_vertical_speed_dms_;
    r.baro_valid         = sample.baro_valid;
    r.gps                = sample.gps;               // bruto (user story 20)
    r.gps_uart_overflows = sample.gps_uart_overflows;
    r.accel_mg[0]        = sample.accel_mg[0];        // bruto (user story 20/29)
    r.accel_mg[1]        = sample.accel_mg[1];
    r.accel_mg[2]        = sample.accel_mg[2];
    r.gyro_ddps[0]       = sample.gyro_ddps[0];
    r.gyro_ddps[1]       = sample.gyro_ddps[1];
    r.gyro_ddps[2]       = sample.gyro_ddps[2];
    r.phase              = FlightPhase::Flight;       // fixo nesta fatia

    // Posição fundida = bruta do GPS: não há fusão nesta barra (issue 05). Fica ao
    // LADO da bruta (r.gps), não no lugar dela — o log_codec grava as duas. A fonte
    // do registro acompanha o pacote: GPS com fix, nenhuma sem. A máquina completa
    // de fonte (Ins/LastValid) é da issue 08.
    r.fused_latitude_1e7  = gps.latitude_1e7;
    r.fused_longitude_1e7 = gps.longitude_1e7;
    r.position_source     = gps_fix_valid ? PositionSource::Gps : PositionSource::None;

    // Byte de saúde honesto (issue 08). Bit em 1 = subsistema OK; o mesmo bitmap vai
    // ao pacote e ao registro, salvo o bit 7 (kAccelSat), que grava a saturação do
    // acelerômetro SÓ no cartão — o byte de saúde do ar é contrato congelado com o
    // solo e não pode ganhar significado novo. Bit 4 (E22) fica sempre 0: o rádio foi
    // abandonado, mas o bit segue reservado para não mexer no layout que o solo lê.
    //
    // As fontes de cada bit: imu e gps vêm da amostra (lidos a cada volta, não
    // piscam); baro, sd e sx1276 vêm de `io` (a amostra não os expõe de forma estável
    // — ver IoSubsystemHealth). Sem GPS-altitude reusada no fallback, "altitude é
    // barométrica" (bit 6) é o mesmo sinal de "baro vivo" (bit 1): io.baro apaga os
    // dois juntos quando o baro morre.
    uint8_t packet_health = 0;
    if (sample.imu_valid) {
        packet_health |= health_bit::kImu;      // bit 0
    }
    if (io.baro) {
        packet_health |= health_bit::kBaro;     // bit 1
        packet_health |= health_bit::kAltRef;   // bit 6
    }
    if (gps.receiving) {
        packet_health |= health_bit::kGps;      // bit 2 — VIVO (fala NMEA), não "tem fix"
    }
    if (io.sd) {
        packet_health |= health_bit::kSd;       // bit 3
    }
    if (io.sx1276) {
        packet_health |= health_bit::kSx1276;   // bit 5
    }
    uint8_t record_health = packet_health;
    if (sample.accel_saturated) {
        record_health |= health_bit::kAccelSat;  // bit 7 — só no registro
    }
    r.health = record_health;

    if (!emitted_once_ || static_cast<int32_t>(t_ms - next_tx_ms_) >= 0) {
        emitted_once_ = true;
        next_tx_ms_   = t_ms + kTxPeriodMs;

        TelemetryPacket& p   = out.packet;
        p.radio              = Radio::Sx1276;
        p.sequence           = packet_sequence_++;
        p.t_ds               = to_decaseconds(t_ms);
        p.altitude_m         = saturate_i16(last_altitude_m_);
        p.vertical_speed_dms = last_vertical_speed_dms_;
        p.phase              = FlightPhase::Flight;   // fixo nesta fatia
        p.health             = packet_health;         // sem o bit 7 de saturação

        // Satélites/HDOP/qualidade seguem no pacote nas DUAS formas — o solo os lê
        // sempre. Só lat/lon e a fonte dependem do fix.
        p.satellites  = gps.satellites;
        p.hdop_half   = gps.hdop_half;
        p.fix_quality = gps.fix_quality;
        if (gps_fix_valid) {
            p.form            = PacketForm::Full;         // 20 B
            p.latitude_1e7    = gps.latitude_1e7;
            p.longitude_1e7   = gps.longitude_1e7;
            p.position_source = PositionSource::Gps;
        } else {
            p.form            = PacketForm::AltitudeOnly;  // 12 B, sem posição
            p.position_source = PositionSource::None;
        }
        out.has_packet       = true;
    }

    return out;
}

}  // namespace core
