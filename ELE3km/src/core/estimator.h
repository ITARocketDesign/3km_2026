// core/estimator.h — estimador de dois canais com sanidade numérica.
//
// Substitui a altitude barométrica direta por uma estimativa FUNDIDA (issue 07).
// Dois canais independentes, cada um um filtro de Kalman cinemático de dois
// estados (posição, velocidade):
//
//   • VERTICAL — altitude e velocidade vertical. Predição pela aceleração da IMU,
//     correção pelo barômetro e pela altitude do GPS.
//   • HORIZONTAL — posição e velocidade num plano tangente local. Correção pelo
//     GPS. A predição NÃO usa a aceleração da IMU: sem magnetômetro o heading não
//     é observável, então a aceleração de corpo não se rotaciona para o referencial
//     de navegação. Horizontalmente o filtro é uma ponte de velocidade constante
//     entre fixes, com ruído de processo alto — é por isso que a altitude é o
//     payload prioritário e a posição não é (PRD §1, a assimetria que estrutura o
//     projeto).
//
// Puro: nenhum header de Arduino, nenhum relógio global, nenhuma alocação. O tempo
// entra como parâmetro. Tudo — filtro, sanidade e caminho de reset — é testável no
// host, e nenhum teste inspeciona a matriz de covariância: só o comportamento.
//
// ── Sanidade numérica a cada passo (PRD §15) ────────────────────────────────
//
// Depois de cada predict e de cada update, o vetor de estado e a diagonal da
// covariância passam por isfinite() e por um limite físico. Divergência detectada
// reseta o canal para a última medição válida (GPS para posição, barômetro para
// altitude), reinicializa a covariância com incerteza máxima, e deixa reconverger —
// o mesmo caminho do boot. O contador de resets é exposto e vai ao log; se subir
// durante o voo, o modelo do filtro precisa de revisão.
#pragma once

#include <stdint.h>

#include "core/types.h"

namespace core {

struct EstimatorConfig {
    // ── Ruído de processo: variância da aceleração não modelada, por canal ──
    // Horizontal é grande de propósito: a predição não tem aceleração de entrada,
    // então todo o movimento entre fixes é ruído de processo.
    float vertical_accel_var = 25.0f;      // (m/s²)²
    float horizontal_accel_var = 100.0f;   // (m/s²)²

    // ── R do barômetro, função da velocidade vertical estimada (H12) ────────
    // Peso alto (variância baixa) abaixo de baro_speed_low; peso quase nulo acima
    // de baro_speed_high, onde a leitura barométrica é ruim. Interpolação linear
    // da variância entre os dois. Durante o boost a altitude vem essencialmente da
    // integração vertical do acelerômetro.
    float baro_r_low = 4.0f;               // m²  (σ≈2 m) a baixa velocidade
    float baro_r_high = 1.0e6f;            // m²  (σ≈1000 m) a alta velocidade
    float baro_speed_low_ms = 30.0f;
    float baro_speed_high_ms = 100.0f;

    // R da altitude do GPS: correção fraca, que só sustenta o canal quando o
    // barômetro sai. O GPS é ruim em altitude comparado ao barômetro em subsônico.
    float gps_alt_r = 100.0f;              // m²  (σ≈10 m)

    // R da posição horizontal do GPS.
    float gps_pos_r = 25.0f;               // m²  (σ≈5 m)

    // ── Rejeição de degraus impossíveis por limite de taxa de variação ──────
    // Um salto de altitude ou de posição que implique uma taxa acima disto entre
    // medições é físico-impossível: a medição é rejeitada em vez de alimentada ao
    // filtro. Degrau de pressão vira degrau de altitude pela ISA; o limite mora
    // aqui, no domínio de altitude, onde o filtro trabalha.
    float max_alt_rate_ms = 400.0f;        // m/s
    float max_pos_rate_ms = 400.0f;        // m/s

    // ── Limites físicos na diagonal (PRD §15) ───────────────────────────────
    float max_horizontal_var = 1.0e8f;     // m²   incerteza > 10 km = inútil
    float max_altitude_var = 2.5e7f;       // m²   (altitude máxima plausível)²
    float max_velocity_var = 2.5e5f;       // m²/s² acima da velocidade do veículo

    // Covariância inicial (e de reset) de cada estado: incerteza máxima, para o
    // filtro reconvergir a partir da primeira medição. Abaixo dos limites acima,
    // senão o próprio reset dispararia divergência.
    float reset_position_var = 1.0e6f;     // m²
    float reset_velocity_var = 1.0e4f;     // m²/s²
};

// A saída de cada ciclo. Tudo o que o FlightComputer precisa para o pacote e o log
// sai daqui; nada de matriz de covariância é exposto — só a estimativa e a saúde.
struct EstimatorOutput {
    bool  has_altitude = false;
    float altitude_m = 0.0f;
    float vertical_speed_ms = 0.0f;
    bool  altitude_from_gps = false;   // barômetro ausente → altitude veio do GPS

    bool    has_horizontal = false;
    int32_t latitude_1e7 = 0;
    int32_t longitude_1e7 = 0;

    // Incerteza reportada de cada canal (desvio-padrão de posição, em metros): a
    // raiz da variância de posição. É saída pública — o que um receptor reporta
    // como precisão —, não a matriz interna: durante um vão de GPS ela cresce, e é
    // por ela que se observa a propagação em predição sem abrir a covariância.
    float horizontal_uncertainty_m = 0.0f;
    float vertical_uncertainty_m = 0.0f;

    // Gps logo após uma correção de GPS; Ins enquanto corre só em predição; None
    // antes do primeiro fix. A máquina de fonte completa (LastValid com idade) é
    // da issue 08 — esta fatia distingue só corrigido de propagado.
    PositionSource source = PositionSource::None;

    // Quantas vezes um canal divergiu e foi resetado desde o boot. Vai ao log.
    uint8_t resets = 0;
};

// O que o estimador consome por ciclo. O FlightComputer monta a partir da amostra
// de sensores e da altitude derivada da máquina de fases; a suíte nativa monta
// direto. Todas as medições vêm com um bit de presença: ausência não é zero.
struct EstimatorInput {
    // Medição de altitude barométrica (relativa à referência de solo), quando há
    // leitura de barômetro fresca nesta amostra.
    bool  have_baro = false;
    float baro_altitude_m = 0.0f;

    // O barômetro EXISTE no sistema. Falso é o hardware ausente/morto no boot: a
    // altitude cai para a do GPS e o estimador sinaliza altitude_from_gps.
    bool  baro_present = true;

    // Fix de GPS válido nesta amostra: altitude MSL e posição.
    bool    have_gps = false;
    float   gps_altitude_m = 0.0f;
    int32_t gps_lat_1e7 = 0;
    int32_t gps_lon_1e7 = 0;

    // Aceleração vertical no referencial de navegação (m/s², já compensada de
    // gravidade), usada na predição do canal vertical. Só entra se imu_valid e não
    // saturada; saturada infla o ruído de predição em vez de integrar.
    bool  imu_valid = false;
    bool  imu_saturated = false;
    float vertical_accel_ms2 = 0.0f;
};

class Estimator {
  public:
    Estimator() = default;
    explicit Estimator(const EstimatorConfig& config) : config_(config) {}

    // Consome uma medição adquirida em t_ms (ms monotônicos desde o boot) e
    // devolve a estimativa corrente. O primeiro ciclo só inicializa; a predição
    // precisa de um Δt, que só existe a partir do segundo.
    EstimatorOutput update(const EstimatorInput& input, uint32_t t_ms);

    uint8_t resets() const { return resets_; }

  private:
    // Um canal cinemático de dois estados, com sua própria covariância 2×2
    // simétrica. Reusado pelo vertical e pelos dois eixos horizontais.
    struct Channel {
        float p = 0.0f;    // posição
        float v = 0.0f;    // velocidade
        float P00 = 0.0f;  // covariância, simétrica: [[P00, P01],[P01, P11]]
        float P01 = 0.0f;
        float P11 = 0.0f;
        bool  initialized = false;

        void predict(float dt, float accel, float accel_var);
        void update(float z, float R);
        void reset(float z, float position_var, float velocity_var);
        // isfinite() no estado e na diagonal, e limite físico nas duas.
        bool healthy(float max_position_var, float max_velocity_var) const;
    };

    EstimatorConfig config_;

    Channel vertical_;
    Channel north_;  // plano tangente local, metros a partir da origem
    Channel east_;

    // Origem do plano tangente: o primeiro fix de GPS válido. Metros por grau de
    // longitude dependem da latitude, então guardam-se os dois fatores no ancoramento.
    bool    have_origin_ = false;
    int32_t origin_lat_1e7 = 0;
    int32_t origin_lon_1e7 = 0;
    float   meters_per_deg_lat_ = 0.0f;
    float   meters_per_deg_lon_ = 0.0f;

    // Última medição válida de cada domínio, para o reset se ancorar nela.
    bool  have_last_baro_ = false;
    float last_baro_altitude_m_ = 0.0f;
    bool  have_last_gps_alt_ = false;
    float last_gps_altitude_m_ = 0.0f;

    // Rejeição de degrau: última medição aceita e seu instante, por domínio.
    bool     have_prev_baro_ = false;
    float    prev_baro_altitude_m_ = 0.0f;
    uint32_t prev_baro_ms_ = 0;

    bool     have_gps_corrected_ = false;  // já houve uma correção de GPS (para a fonte)

    // Rejeição de salto: última posição de GPS ACEITA (metros no plano tangente) e
    // seu instante. A taxa é medida entre fixes consecutivos, no intervalo real
    // entre eles — não contra a estimativa atual num único ciclo, senão um fix
    // legítimo depois de um vão pareceria um salto instantâneo e seria rejeitado.
    bool     have_prev_gps_ = false;
    float    prev_gps_north_m_ = 0.0f;
    float    prev_gps_east_m_ = 0.0f;
    uint32_t prev_gps_ms_ = 0;

    uint32_t last_t_ms_ = 0;
    bool     have_last_t_ = false;

    uint8_t resets_ = 0;
};

}  // namespace core
