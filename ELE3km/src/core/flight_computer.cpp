#include "core/flight_computer.h"

#include <cmath>

namespace core {
namespace {

// Comparação de instantes que sobrevive ao wrap de uint32_t (~49 dias): o tempo
// entra como parâmetro e nunca é comparado direto.
bool reached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

int16_t to_meters(float altitude_m) {
    const float rounded = std::round(altitude_m);
    if (rounded > 32767.0f) return 32767;
    if (rounded < -32768.0f) return -32768;
    return static_cast<int16_t>(rounded);
}

uint16_t to_decisegundos(uint32_t t_ms) {
    const uint32_t ds = t_ms / 100u;
    return ds > 65535u ? static_cast<uint16_t>(65535) : static_cast<uint16_t>(ds);
}

int16_t to_decimetros_por_s(float speed_ms) {
    const float dms = std::round(speed_ms * 10.0f);
    if (dms > 32767.0f) return 32767;
    if (dms < -32768.0f) return -32768;
    return static_cast<int16_t>(dms);
}

// Aceleração vertical de navegação a partir do acelerômetro de corpo, para a
// predição do canal vertical. Sem atitude, aproxima-se o eixo z de corpo pelo
// vertical e subtrai-se 1 g: é grosseira fora da rampa, mas é justamente na rampa
// e no boost — quando o foguete está alinhado com o trilho — que a integração
// vertical importa, porque é lá que o barômetro é ruim (H12). O log guarda os três
// eixos brutos ao lado, então a decisão pode ser refeita depois.
constexpr float kGravityMs2 = 9.80665f;
float vertical_accel_ms2_from_body(const int16_t accel_mg[3]) {
    return (static_cast<float>(accel_mg[2]) - 1000.0f) * kGravityMs2 / 1000.0f;
}

}  // namespace

UpdateResult FlightComputer::update(const SensorSample& sample, uint32_t t_ms,
                                    bool write_in_progress,
                                    const IoSubsystemHealth& io_health,
                                    PhaseOverride phase_override) {
    // A fase, a referência barométrica e a altitude derivada agora são da máquina
    // de fases (issue 06). Ela mantém a última altitude quando o barômetro falha;
    // a propagação inercial sem barômetro é da issue 07.
    PhaseState phase = phase_.update(sample, t_ms);

    // Sobrescrita de bancada (não-Auto): pina só a FASE que as regras a jusante
    // enxergam. Referência, altitude, velocidade, liftoff e posição continuam sendo
    // os da máquina real — é isso que permite conferir os sensores em cada fase.
    if (phase_override != PhaseOverride::Auto) {
        phase.phase = phase_override == PhaseOverride::Rail   ? FlightPhase::Rail
                    : phase_override == PhaseOverride::Flight ? FlightPhase::Flight
                                                             : FlightPhase::Landed;
    }

    // O estimador funde a altitude derivada da fase (a medição barométrica do canal
    // vertical) com a aceleração da IMU e o GPS. Ele é a fonte da velocidade
    // vertical, da posição fundida e do contador de resets; a altitude observável
    // do pacote continua sendo a da fase (não há campo de altitude fundida no log,
    // e a altitude é o payload prioritário — ver Estado da implementação da 07).
    // Vivacidade do barômetro para o bit de saúde. O bit é "subsistema OK" (alive),
    // não "amostra fresca neste ciclo": como o baro é lido a 25 Hz num ciclo de 100
    // Hz, sample.baro_valid é falso em 3 de 4 ciclos mesmo sadio. Latcheamos o
    // instante da última leitura válida e consideramos o baro vivo se ela foi
    // recente — kBaroHealthMs cobre com folga o período de 40 ms do baro (e alguns
    // ciclos perdidos numa recuperação de barramento) e só cai se ele parar de
    // verdade. O estimador continua usando sample.baro_valid cru (só funde amostra
    // fresca); apenas o bit de saúde usa a versão latcheada.
    if (sample.baro_valid) {
        have_baro_valid_    = true;
        last_baro_valid_ms_ = t_ms;
    }
    constexpr uint32_t kBaroHealthMs = 200;
    const bool baro_alive =
        have_baro_valid_ && (t_ms - last_baro_valid_ms_) <= kBaroHealthMs;

    EstimatorInput est_in;
    est_in.have_baro        = sample.baro_valid;
    est_in.baro_altitude_m  = phase.altitude_m;
    est_in.baro_present     = phase.has_reference;
    est_in.have_gps         = sample.gps.valid;
    est_in.gps_altitude_m   = sample.gps.altitude_m;
    est_in.gps_lat_1e7      = sample.gps.latitude_1e7;
    est_in.gps_lon_1e7      = sample.gps.longitude_1e7;
    est_in.imu_valid        = sample.imu_valid;
    est_in.imu_saturated    = sample.accel_saturated;
    est_in.vertical_accel_ms2 =
        sample.imu_valid ? vertical_accel_ms2_from_body(sample.accel_mg) : 0.0f;
    const EstimatorOutput est = estimator_.update(est_in, t_ms);

    // A máquina de fonte de posição (issue 08) decide de onde vem a posição do
    // pacote e, com a flag de confiança, a forma dele. Ela consome a posição
    // FUNDIDA do estimador (o que se transmite em GPS e na ponte Ins) e o último fix
    // BRUTO (o que se transmite em LastValid). A ponte inercial só existe se houver
    // IMU viva: sem ela, a perda de GPS cai direto para a última válida.
    if (sample.imu_valid) imu_ever_valid_ = true;
    PositionSourceInput pos_in;
    pos_in.have_gps_fix = sample.gps.valid;
    pos_in.gps_lat_1e7  = sample.gps.latitude_1e7;
    pos_in.gps_lon_1e7  = sample.gps.longitude_1e7;
    pos_in.has_fused    = est.has_horizontal;
    pos_in.fused_lat_1e7 = est.latitude_1e7;
    pos_in.fused_lon_1e7 = est.longitude_1e7;
    pos_in.imu_present  = imu_ever_valid_;
    // Em POUSADO a máquina troca de regime: filtra os fixes (satélites ≥ 4, HDOP ≤
    // 5,0) e transmite a média dos aceitos (issue 13).
    pos_in.is_landed    = (phase.phase == FlightPhase::Landed);
    pos_in.satellites   = sample.gps.satellites;
    pos_in.hdop_half    = sample.gps.hdop_half;
    const PositionSourceOutput pos = position_source_.update(pos_in, t_ms);

    // Barômetro ausente (nunca houve referência) faz a altitude cair para a do GPS,
    // sinalizada pelo bit kAltRef apagado, como no fallback de referência absurda.
    const bool altitude_from_gps = phase.altitude_from_gps || est.altitude_from_gps;
    float altitude_for_output = phase.altitude_m;
    if (est.altitude_from_gps && sample.gps.valid) {
        altitude_for_output = sample.gps.altitude_m;
    }
    const int16_t vertical_speed_dms = to_decimetros_por_s(est.vertical_speed_ms);

    UpdateResult result;
    result.log.t_ms               = t_ms;
    result.log.sequence           = log_sequence_++;
    result.log.pressure_pa        = sample.pressure_pa;
    result.log.temperature_c      = sample.temperature_c;
    result.log.altitude_m         = altitude_for_output;
    result.log.baro_valid         = sample.baro_valid;
    result.log.gps                = sample.gps;
    result.log.gps_uart_overflows = sample.gps_uart_overflows;
    result.log.phase              = phase.phase;
    result.log.position_source    = pos.source;
    result.persist_phase          = phase.persist_now;

    // Cadência de log (issue 13). Em voo grava-se todo ciclo; em POUSADO cai para
    // 1 Hz, senão o beacon grava horas de um foguete parado. A telemetria segue a
    // própria cadência no escalonador e não é afetada por isto.
    if (phase.phase == FlightPhase::Landed) {
        if (has_logged_landed_ &&
            !reached(t_ms, last_landed_log_ms_ + config_.landed_log_period_ms)) {
            result.should_log = false;
        } else {
            has_logged_landed_ = true;
            last_landed_log_ms_ = t_ms;
        }
    }

    // Bruto da IMU e saída fundida no log, ao lado dos brutos (user story 103).
    for (int i = 0; i < 3; ++i) {
        result.log.accel_mg[i]  = sample.accel_mg[i];
        result.log.gyro_ddps[i] = sample.gyro_ddps[i];
    }
    result.log.vertical_speed_dms = vertical_speed_dms;
    result.log.estimator_resets   = est.resets;
    if (est.has_horizontal) {
        result.log.fused_latitude_1e7  = est.latitude_1e7;
        result.log.fused_longitude_1e7 = est.longitude_1e7;
    }

    // O candidato é montado a cada amostra, e é o escalonador que decide se
    // algum rádio o põe no ar agora. Os campos `radio` e `sequence` são dele.
    //
    // O campo de tempo passa a ser decissegundos desde o LIFTOFF (PACKET_FORMAT):
    // antes do liftoff, desde o boot. O contrato com o solo é que o zero do tempo
    // é a ignição, para o gráfico de voo não ter um offset arbitrário de rampa.
    TelemetryPacket candidate;
    candidate.t_ds =
        phase.has_liftoff ? to_decisegundos(t_ms - phase.liftoff_ms) : to_decisegundos(t_ms);
    candidate.altitude_m = to_meters(altitude_for_output);
    candidate.phase      = phase.phase;
    candidate.vertical_speed_dms = vertical_speed_dms;  // do estimador (issue 07)

    // A forma do pacote e a fonte vêm da máquina de fonte de posição (issue 08): há
    // posição em que confiar → pacote completo com a fonte (GPS fresco, ponte Ins,
    // ou última válida); sem nenhuma posição ainda → só-altitude. A idade do fix
    // fica na saída da máquina e vai ao log; ela NÃO cabe nos 20 B do ar, onde a
    // própria fonte LastValid já sinaliza que a posição é velha (PACKET_FORMAT.md).
    candidate.position_source = pos.source;
    if (pos.confident) {
        candidate.form          = PacketForm::Full;
        candidate.latitude_1e7  = pos.latitude_1e7;
        candidate.longitude_1e7 = pos.longitude_1e7;
    } else {
        candidate.form          = PacketForm::AltitudeOnly;
    }
    // Satélites, HDOP e qualidade vão nas DUAS formas: sem fix eles são a única
    // pista que o solo tem de quão perto o receptor está de voltar.
    //
    // Em POUSADO os 3 bits de qualidade de fix mudam de significado: passam a
    // carregar o NÚMERO DE AMOSTRAS pós-pouso acumuladas (issue 13). A fase, também
    // no byte de flags, é o que diz ao solo qual dos dois sentidos ler — em voo é a
    // qualidade da GGA, pousado é a contagem de amostras (0–7). É overload de campo,
    // não campo novo: o formato de 20 B é orçamento gasto e não pode crescer.
    candidate.fix_quality = (phase.phase == FlightPhase::Landed)
                                ? pos.samples
                                : sample.gps.fix_quality;
    candidate.satellites  = sample.gps.satellites;
    candidate.hdop_half   = sample.gps.hdop_half;

    // O bitmap de saúde {imu, baro, gps, sd, sx1276}, mais o kAltRef (issue 10).
    // imu/baro/gps vêm da amostra; a saúde do cartão e do rádio vem da task de I/O
    // pelo io_health. O bit 4 (era o E22) fica sempre em 0, reservado no layout. É
    // por este byte que a equipe de solo diagnostica do chão o que falhou a bordo.
    //
    // Os bits são "subsistema vivo", não "amostra fresca neste ciclo". O do GPS é
    // sobre o receptor estar vivo, não sobre haver fix: um receptor falando NMEA sem
    // fix é saudável debaixo de um céu ruim. O da IMU é a validade da leitura desta
    // amostra — mas a IMU é lida todo ciclo (100 Hz), então isso já é vivacidade. O
    // do baro usa o latch baro_alive acima (25 Hz num ciclo de 100 Hz), senão
    // piscaria zero mesmo com a peça sadia.
    //
    // O bit kAltRef diz que a altitude é relativa à referência barométrica e é
    // confiável; ele cai a zero quando a referência reusada foi julgada absurda e
    // a altitude passou a vir do GPS (H4.4).
    candidate.health = static_cast<uint8_t>((sample.imu_valid ? health_bit::kImu : 0) |
                                            (baro_alive ? health_bit::kBaro : 0) |
                                            (sample.gps.receiving ? health_bit::kGps : 0) |
                                            (io_health.sd ? health_bit::kSd : 0) |
                                            (io_health.sx1276 ? health_bit::kSx1276 : 0) |
                                            (altitude_from_gps ? 0 : health_bit::kAltRef));

    // O MESMO bitmap vai ao registro de log, não só ao pacote (issue 10): é dele
    // que a análise pós-voo reconstrói a saúde de cada ciclo a partir do cartão.
    result.log.health = candidate.health;

    TxSchedulerInput input;
    input.candidate         = candidate;
    input.write_in_progress = write_in_progress;

    const TxSchedulerDecision decision = scheduler_.update(input, t_ms);
    for (uint8_t i = 0; i < decision.transmission_count; ++i) {
        result.packets[i] = decision.transmissions[i];
    }
    result.packet_count = decision.transmission_count;

    return result;
}

}  // namespace core
