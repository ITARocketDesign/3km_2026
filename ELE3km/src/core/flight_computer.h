// core/flight_computer.h — o orquestrador do núcleo.
//
// É o seam primário de teste (PRD, Testing Decisions): recebe uma amostra e o
// tempo, devolve um registro de log e a lista de pacotes a transmitir. A maioria
// dos testes entra por aqui.
//
// O tempo entra como parâmetro. A classe não lê relógio nenhum, não aloca, e não
// depende de nada global.
#pragma once

#include <stdint.h>

#include "core/estimator.h"
#include "core/flight_phase.h"
#include "core/position_source.h"
#include "core/tx_scheduler.h"
#include "core/types.h"

namespace core {

struct UpdateResult {
    LogRecord       log;
    TelemetryPacket packets[kMaxPacketsPerUpdate];
    uint8_t         packet_count = 0;

    // true no ciclo em que a máquina de fases pede persistência (liftoff ou
    // transição de fase). A HAL grava o snapshot da fase em NVS; poucas escritas
    // por voo. Ver core/flight_phase.h.
    bool persist_phase = false;

    // true quando este registro deve ser gravado no cartão. Em voo é sempre true —
    // nenhuma amostra é descartada. Em POUSADO cai para 1 Hz (issue 13): sem isso o
    // beacon gravaria horas de um foguete parado a 6,4 kB/s. A telemetria NÃO é
    // afetada — ela tem a própria cadência no escalonador; isto é só o log.
    bool should_log = true;
};

struct FlightComputerConfig {
    // Quando e por qual rádio é decisão do escalonador; aqui só se passa adiante
    // a configuração dele.
    TxSchedulerConfig tx;

    // Limiares da máquina de fases e da referência barométrica.
    PhaseConfig phase;

    // Ganhos e limites do estimador de dois canais (issue 07).
    EstimatorConfig estimator;

    // Limiar de frescor e janela de ponte inercial da fonte de posição (issue 08).
    PositionSourceConfig position_source;

    // Período mínimo entre gravações de log em POUSADO (issue 13). Fora de POUSADO
    // grava-se todo ciclo; pousado, um registro por segundo.
    uint32_t landed_log_period_ms = 1000;
};

// Sobrescrita de fase para BANCADA (issue de teste manual, não de voo). Auto = a
// máquina de fases real decide, e é o único valor que um build de voo usa. Os
// outros três pinam a fase que as regras a jusante enxergam (cadência de log,
// média pós-pouso, bits de fase/amostras do pacote) SEM tocar em referência,
// altitude, velocidade ou posição — esses continuam vindo dos sensores reais.
// Não é campo do contrato do ar: o valor Auto nunca vai ao pacote; só troca qual
// FlightPhase o resto do update() vê. Entra por parâmetro, não por estado da
// classe: quem detém o modo é a HAL (via Serial), o núcleo só o aplica por ciclo.
enum class PhaseOverride : uint8_t { Auto, Rail, Flight, Landed };

class FlightComputer {
  public:
    FlightComputer() = default;
    explicit FlightComputer(const FlightComputerConfig& config)
        : config_(config),
          scheduler_(config.tx),
          phase_(config.phase),
          estimator_(config.estimator),
          position_source_(config.position_source) {}

    // Restaura a fase e a referência de um reset (issue 06). A HAL lê o snapshot
    // da NVS e o motivo do reset e os passa adiante; a decisão de reusar é do
    // núcleo. Sem chamada, o default é um voo novo na rampa.
    void begin(const PhaseRestore& restore) { phase_.begin(restore); }

    // Consome uma amostra adquirida em t_ms (ms monotônicos desde o boot) e
    // devolve o que deve ser gravado e o que deve ir ao ar.
    //
    // `write_in_progress` é o único estado do mundo que entra aqui sem ser
    // sensor: uma escrita no cartão em andamento tem o barramento SPI na mão, e
    // nenhuma transmissão pode interrompê-la. O default é o caso honesto de quem
    // não tem cartão — e é o que mantém legível a maioria dos testes, que não
    // têm nada a ver com o cartão.
    // `io_health` é a saúde do cartão e do rádio, que a task de I/O possui e que
    // não chega pela amostra. O default (tudo falso) mantém legíveis os testes que
    // não têm nada a ver com esses subsistemas: os bits ficam apagados.
    // `phase_override` é só de bancada: Auto (o default) deixa a máquina de fases
    // decidir, como em voo; qualquer outro valor pina a fase a jusante para testar
    // cada fase no metal com sensores reais. Ver PhaseOverride acima.
    UpdateResult update(const SensorSample& sample, uint32_t t_ms,
                        bool write_in_progress = false,
                        const IoSubsystemHealth& io_health = {},
                        PhaseOverride phase_override = PhaseOverride::Auto);

    // O snapshot a persistir quando UpdateResult::persist_phase sobe.
    PhaseSnapshot phase_snapshot() const { return phase_.snapshot(); }

  private:
    FlightComputerConfig config_;

    // Dono de QUANDO, de QUAL RÁDIO e do número de sequência. Esta classe só
    // decide o CONTEÚDO.
    TxScheduler scheduler_;

    // Dono da fase, da referência barométrica e da altitude derivada (issue 06).
    PhaseEstimator phase_;

    // Fusão de dois canais: dono da velocidade vertical, da posição fundida e do
    // contador de resets (issue 07). Consome a altitude derivada da fase como a
    // medição barométrica do canal vertical.
    Estimator estimator_;

    // Decide a FONTE da posição (GPS ↔ INS ↔ última válida) e, com ela, a forma do
    // pacote (issue 08). Consome a posição fundida do estimador e o último fix bruto.
    PositionSourceMachine position_source_;

    // A IMU já produziu uma amostra válida em algum momento. Falso é IMU ausente ou
    // morta no boot, que desabilita a ponte inercial da fonte de posição (issue 08).
    bool imu_ever_valid_ = false;

    uint32_t log_sequence_ = 0;

    // Cadência de log em POUSADO (issue 13): o instante do último registro gravado e
    // se já houve um. POUSADO é de mão única, então o latch não precisa ser rearmado.
    bool     has_logged_landed_ = false;
    uint32_t last_landed_log_ms_ = 0;

    // Vivacidade do barômetro para o bit de saúde (PACKET_FORMAT: "subsistema OK",
    // não "amostra fresca neste ciclo"). O barômetro é lido a 25 Hz dentro do ciclo
    // de 100 Hz, então baro_valid é falso em 3 de cada 4 ciclos mesmo com a peça
    // sadia; usá-lo direto no bit faria a saúde piscar 0. Aqui guardamos o instante
    // da última leitura válida e o bit reflete "válido nos últimos kBaroHealthMs".
    bool     have_baro_valid_ = false;
    uint32_t last_baro_valid_ms_ = 0;
};

}  // namespace core
