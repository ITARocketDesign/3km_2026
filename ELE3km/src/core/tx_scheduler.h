// core/tx_scheduler.h — escalonador de arbitragem do caminho de rádio.
//
// Recebe estado e tempo, devolve a próxima ação (PRD §20). É o seam que torna as
// invariantes de recurso testáveis no host, e é por isso que ele mora no núcleo
// puro e não dentro da task de I/O: as tasks são bombas burras que executam o
// que este arquivo decidiu.
//
// ── Um único rádio ──────────────────────────────────────────────────────────
//
// Só o SX1276 onboard transmite. O E22 de 433 MHz foi removido do projeto
// (Docs/ELE3km_drop_e22_single_radio.md): o bring-up de bancada falhou por trilha
// de MOSI aberta até o pad 17 e o rail U6 vinha a 3,4 V, acima dos 3,30 V do doc.
// Sem o segundo rádio, o link é single-band; a decisão está registrada no doc.
//
// A interface não tem nenhum parâmetro de saúde de rádio — a ausência é
// deliberada: um rádio que RadioLib reporta como são pode estar com a antena
// solta ou o PA queimado, e o firmware não distingue. A falha aparece nos dados,
// no chão, não num failover que não teria como disparar.
//
// ── Divisão de responsabilidade com o FlightComputer ────────────────────────
//
// O escalonador é dono de QUANDO e do NÚMERO DE SEQUÊNCIA. O FlightComputer é dono
// do CONTEÚDO. A sequência é artefato de agendamento, não de dado: ela anda uma vez
// por ciclo de telemetria.
#pragma once

#include <stdint.h>

#include "core/types.h"

namespace core {

// Um único rádio: no máximo uma transmissão liberada por update.
constexpr uint8_t kMaxPacketsPerUpdate = 1;

// Quantas transmissões cabem numa janela. Dimensionado para que o TETO sempre
// morda primeiro: com o orçamento de 20 % de 10 s (2000 ms) e o airtime de 78 ms
// do SX1276, cabem 25 transmissões na janela. Um anel cheio recusa a transmissão —
// é fail-safe, erra recusando.
constexpr uint8_t kDutyWindowEntries = 32;

// Orçamento do rádio na janela deslizante.
struct RadioBudget {
    // Airtime cobrado por transmissão. Cobra-se sempre o do pacote COMPLETO
    // (20 B): a forma só-altitude é mais barata, então cobrar o valor cheio erra
    // para o lado de gastar orçamento demais, que é o lado seguro, e evita uma
    // tabela de airtime por forma dentro do núcleo.
    uint16_t airtime_ms;

    // Teto de ciclo de trabalho, em permilagem da janela. Constante VERIFICADA,
    // não comentário (hazard H1c).
    uint16_t duty_permille;
};

// FailActive e o comportamento normal de voo. SurvivalBeacon e a unica excecao:
// SX1276 em SF12 / 20 s como beacon pos-pouso.
enum class TxMode : uint8_t {
    FailActive,
    SurvivalBeacon,
};

struct TxSchedulerConfig {
    TxMode mode = TxMode::FailActive;

    // Cadência do ciclo de telemetria em voo (PRD §5: 1 Hz).
    uint32_t cycle_period_ms = 1000;

    // Separação mínima entre duas transmissões, seja qual for o agendamento
    // nominal. É a forma executável de "verificar em vez de presumir": no voo
    // nominal ela nunca dispara, porque os ciclos distam 1 s; ela existe como
    // backstop para o dia em que a lógica de cadência estiver errada.
    //
    // 200 ms cobre com margem o airtime do rádio (78 ms em SF7 com 20 B), então
    // duas transmissões separadas por este intervalo não se sobrepõem no ar.
    uint32_t min_gap_ms = 200;

    // Janela do teto de ciclo de trabalho. 10 s cobre dez ciclos de telemetria,
    // que é o horizonte em que o aquecimento do PA e o consumo médio da bateria
    // são a grandeza relevante (hardware_constraints §C6).
    uint32_t duty_window_ms = 10000;

    // Orçamento do SX1276 em voo: SF7, 20 B, 78 ms a 1 Hz é 7,8 %; o teto de 20 %
    // dá folga para a cadência errar um pouco e ainda assim ser pega.
    RadioBudget budget = {78, 200};  // 915 MHz, SF7, +20 dBm, rail de 5 V
};

// O estado do mundo que o escalonador precisa saber neste instante.
struct TxSchedulerInput {
    // O pacote que o FlightComputer montou agora. Os campos `radio` e `sequence`
    // são ignorados — quem os preenche é o escalonador.
    TelemetryPacket candidate;

    // true enquanto a HAL do cartão está no meio de uma escrita. Uma escrita
    // NUNCA é interrompida por uma transmissão: aqui a disputa é pelo BARRAMENTO
    // SPI, que a escrita já tem na mão, e que carregar a carga útil do rádio
    // também precisa.
    bool write_in_progress = false;
};

// A próxima ação. As tasks executam isto sem política própria.
struct TxSchedulerDecision {
    TelemetryPacket transmissions[kMaxPacketsPerUpdate];
    uint8_t         transmission_count = 0;
};

class TxScheduler {
  public:
    TxScheduler() = default;
    explicit TxScheduler(const TxSchedulerConfig& config) : config_(config) {}

    // Recebe estado e tempo, devolve a próxima ação.
    //
    // No começo de cada ciclo o candidato é latcheado: é essa cópia, carimbada com
    // o número de sequência do ciclo, que o rádio transmite. SurvivalBeacon usa a
    // mesma mecânica, só com o período de 20 s.
    //
    // ── A regra de exclusão que sobra ───────────────────────────────────────
    //
    // Escrita em andamento ⇒ nenhuma transmissão começa. Disputa de BARRAMENTO: a
    // escrita tem o SPI na mão, e carregar a carga útil do rádio precisa dele. Uma
    // transmissão barrada aqui é adiada, nunca interrompe a escrita.
    //
    // A antiga regra de RAIL (E22 no ar ⇒ nenhuma escrita) morreu com o E22: o
    // SX1276 vive no regulador de 5 V do Heltec, não no rail de 3,3 V do cartão,
    // então o airtime dele nunca bloqueou escrita.
    TxSchedulerDecision update(const TxSchedulerInput& input, uint32_t t_ms);

  private:
    // Uma transmissão agendada e ainda não liberada.
    struct Slot {
        TelemetryPacket packet;
        uint32_t        due_ms = 0;
        bool            pending = false;
    };

    // Janela deslizante de airtime do rádio: um anel de transmissões, das quais só
    // interessam as que ainda estão dentro da janela.
    struct DutyWindow {
        struct Entry {
            uint32_t t_ms;
            uint16_t airtime_ms;
        };
        Entry   entries[kDutyWindowEntries];
        uint8_t head = 0;   // a mais antiga ainda na janela
        uint8_t count = 0;
    };

    // Arma o slot do ciclo que começa em cycle_start_ms.
    void begin_cycle(const TelemetryPacket& candidate, uint32_t cycle_start_ms);

    // O teto cabe? Descarta o que saiu da janela e responde. Não cobra nada.
    bool fits_duty_ceiling(uint32_t t_ms);

    // Cobra o airtime de uma transmissão liberada.
    void charge_duty(uint32_t t_ms);

    uint32_t cycle_period_ms() const;
    RadioBudget budget_for() const;

    TxSchedulerConfig config_;

    bool     started_ = false;        // já houve um primeiro update?
    uint32_t next_cycle_ms_ = 0;      // início do próximo ciclo de telemetria
    uint16_t sequence_ = 0;           // anda uma vez por ciclo; dá a volta em u16

    // Última transmissão liberada.
    bool     has_transmitted_ = false;
    uint32_t last_tx_ms_ = 0;

    // O slot do ciclo. Só sobrevive ao seu ciclo: o ciclo seguinte sobrescreve,
    // então uma transmissão perdida é perdida e nunca se acumula numa fila.
    Slot slot_;

    // Janela deslizante do airtime do rádio.
    DutyWindow window_;
};

}  // namespace core
