// core/flight_phase.h — máquina de fases de voo e referência barométrica.
//
// RAMPA → VOO → POUSADO, no núcleo puro, tempo como parâmetro. É a fatia que dá
// sentido à altitude transmitida: sem uma referência de solo, "altitude" é
// relativa a um zero que ninguém sabe qual era.
//
// ── O apogeu é a armadilha (hardware_constraints C2) ────────────────────────
//
// Em queda livre no apogeu o acelerômetro lê ≈0 g DE FORMA ESTÁVEL; em repouso lê
// ≈1 g. Um detector de pouso por baixa variância vê a mesma assinatura nos dois e
// dispara no apogeu, jogando fora a telemetria da descida inteira — que é a razão
// de existir do projeto. Por isso o pouso exige QUATRO condições simultâneas, e a
// primeira é |a| perto de 1 g, não "aceleração estável".
//
// ── Liftoff por aceleração, referência congelada de 1 s antes ───────────────
//
// O liftoff é por |a| sustentado acima de um limiar (~2,5 g por ~100 ms), nunca
// por altitude — a altitude é justamente o que ainda não se pode medir sem uma
// referência. No instante do liftoff a referência é congelada no valor de ~1 s
// ANTES, lido de um anel de histórico, para não pegar o transiente do motor nem
// manuseio na rampa.
//
// A persistência em NVS e a leitura do motivo do reset são da HAL; a DECISÃO de
// reusar a referência salva mora aqui, e é pura: reusa só se o reset não foi
// power-on limpo E a fase salva era de voo.
#pragma once

#include <stdint.h>

#include "core/types.h"

namespace core {

struct PhaseConfig {
    // Liftoff: |a| acima do limiar, sustentado (D12: ~2,5 g por ~100 ms).
    uint16_t liftoff_accel_mg = 2500;
    uint32_t liftoff_sustain_ms = 100;

    // Referência na rampa: média lenta (EWMA) com constante de tempo de dezenas de
    // segundos, e o quanto antes do liftoff o valor congelado é lido do anel.
    float    reference_tau_ms = 30000.0f;
    uint32_t freeze_lookback_ms = 1000;

    // Pouso, as quatro condições. Nenhuma sozinha declara pouso.
    uint16_t landed_accel_tol_mg = 150;      // |a| perto de 1 g: ||a|-1000| < tol
    float    landed_alt_band_m = 3.0f;       // altitude estável dentro de ±banda…
    uint32_t landed_alt_stable_ms = 10000;   // …por N segundos consecutivos
    float    landed_near_ground_m = 30.0f;   // altitude perto da referência de solo
    uint32_t landed_min_flight_ms = 20000;   // tempo mínimo desde o liftoff

    // Sanidade da referência reusada (H4.4): acima disto em módulo, a altitude que
    // a referência reusada implica é absurda, e cai-se para a altitude do GPS.
    float    absurd_altitude_m = 6000.0f;
};

// O que a HAL persiste em NVS e devolve no boot. A HAL grava e lê os bytes; a
// política de reuso é do núcleo.
struct PhaseSnapshot {
    FlightPhase phase = FlightPhase::Rail;
    float       reference_pa = 0.0f;
    uint32_t    liftoff_ms = 0;          // para o t desde o liftoff sobreviver ao reset
    bool        has_reference = false;
};

// O que a HAL sabe antes do primeiro update: o snapshot salvo e o motivo do reset.
struct PhaseRestore {
    bool          have_saved = false;    // a NVS tinha um snapshot deste ou de outro voo
    PhaseSnapshot saved;
    bool          clean_poweron = true;  // esp_reset_reason() == ESP_RST_POWERON
};

// A saída de cada ciclo. Tudo o que o FlightComputer precisa para montar o log e
// o pacote sai daqui — a fase, a referência, a altitude derivada, e o instante do
// liftoff que ancora o campo de tempo do pacote.
struct PhaseState {
    FlightPhase phase = FlightPhase::Rail;
    bool        has_reference = false;
    float       reference_pa = 0.0f;
    float       altitude_m = 0.0f;         // acima da referência; ou do GPS no fallback
    bool        has_liftoff = false;
    uint32_t    liftoff_ms = 0;
    bool        altitude_from_gps = false; // referência reusada absurda → altitude do GPS
    bool        persist_now = false;       // houve liftoff ou transição de fase neste ciclo
};

class PhaseEstimator {
  public:
    PhaseEstimator() = default;
    explicit PhaseEstimator(const PhaseConfig& config) : config_(config) {}

    // Aplica a regra de reuso no boot. Sem reuso, começa na rampa e constrói a
    // média lenta; com reuso, entra já em voo com a referência salva.
    void begin(const PhaseRestore& restore);

    // Consome uma amostra adquirida em t_ms e devolve fase, referência e altitude.
    PhaseState update(const SensorSample& sample, uint32_t t_ms);

    // O snapshot a gravar quando `persist_now` sobe.
    PhaseSnapshot snapshot() const;

  private:
    // Anel de histórico de pressão para congelar o valor de ~1 s antes do liftoff.
    // Timestamped porque a cadência do barômetro (25 Hz) não é o ciclo (100 Hz).
    static constexpr uint8_t kHistory = 48;   // ~1,9 s a 25 Hz, folga sobre o 1 s

    void   record_pressure(float pressure_pa, uint32_t t_ms);
    float  pressure_at(uint32_t target_ms) const;  // o mais próximo de target_ms

    PhaseConfig config_;

    FlightPhase phase_ = FlightPhase::Rail;
    bool        has_reference_ = false;
    float       reference_pa_ = 0.0f;    // na rampa é a média lenta; congela no liftoff
    float       altitude_m_ = 0.0f;
    bool        altitude_from_gps_ = false;

    bool        has_liftoff_ = false;
    uint32_t    liftoff_ms_ = 0;

    // Reuso de referência na volta de um reset, e a sanidade H4.4 pendente até a
    // primeira leitura de barômetro depois do boot.
    bool        reused_ = false;
    bool        reuse_sanity_pending_ = false;

    // Liftoff: acumula o tempo com |a| acima do limiar.
    bool        above_threshold_ = false;
    uint32_t    above_since_ms_ = 0;

    // Pouso: âncora de estabilidade de altitude.
    float       stable_anchor_m_ = 0.0f;
    uint32_t    stable_since_ms_ = 0;
    bool        has_stable_anchor_ = false;

    // Anel de histórico de pressão.
    struct Sample { uint32_t t_ms; float pressure_pa; };
    Sample   history_[kHistory];
    uint8_t  history_head_ = 0;
    uint8_t  history_count_ = 0;

    uint32_t last_update_ms_ = 0;
    bool     has_last_update_ = false;
};

}  // namespace core
