// core/survival_computer.h — o orquestrador enxuto do fallback.
//
// O análogo de sobrevivência do FlightComputer do ELE3km, sem nada que possa
// entrar no estado errado: sem filtro, sem máquina de fases, sem escalonador, sem
// fonte de posição. Recebe uma amostra e o tempo (ms desde o boot), devolve o
// registro de log (todo ciclo) e — na cadência de 1 Hz — o pacote a transmitir.
//
// Segue o contrato do core/ (types.h): nenhum header de Arduino, nenhum relógio
// global, nenhuma variável global, nenhuma alocação dinâmica. O tempo entra por
// parâmetro.
#pragma once

#include <stdint.h>

#include "core/types.h"

namespace core {

// Datum ISA fixo. A altitude do pacote é pressão-altitude contra este zero; o
// operador subtrai a leitura de rampa no chão (PRD → barra de sobrevivência). Sem
// referência a bordo, sem estado, sem problema de reset em voo.
constexpr float kFixedDatumPa = 101325.0f;

// A telemetria sai a 1 Hz, do power-on à morte da bateria, sem depender de
// detectar liftoff nem pouso.
constexpr uint32_t kTxPeriodMs = 1000;

// A saída de um ciclo: o registro, montado todo ciclo, e — quando a cadência de
// 1 Hz manda — o pacote a transmitir. `has_packet` falso significa "só registro
// neste ciclo".
struct Outputs {
    LogRecord       record;
    TelemetryPacket packet;
    bool            has_packet = false;
};

class SurvivalComputer {
  public:
    // Consome a amostra adquirida em t_ms e devolve o que gravar e — a 1 Hz — o
    // que transmitir. O primeiro ciclo já emite um pacote (telemetria do
    // power-on); depois, um a cada kTxPeriodMs.
    Outputs update(const SensorSample& sample, uint32_t t_ms);

  private:
    // Duas sequências independentes: a do registro incrementa por ciclo (u32); a
    // do pacote, por pacote transmitido (u16 global). Ver types.h.
    uint32_t record_sequence_ = 0;
    uint16_t packet_sequence_ = 0;

    // Cadência de TX baseada no tempo, não em contagem de voltas: robusta ao
    // jitter do laço.
    uint32_t next_tx_ms_ = 0;
    bool     emitted_once_ = false;

    // Última altitude barométrica derivada, mantida entre ciclos: o baro é lido a
    // 25 Hz dentro do laço de 50 Hz, então a maioria dos ciclos não traz leitura
    // fresca. Um pacote não pode sair com altitude 0 só por cair num subciclo sem
    // baro. `have_baro_altitude_` é o bit 6 de saúde: a altitude é barométrica.
    float last_altitude_m_ = 0.0f;
    bool  have_baro_altitude_ = false;

    // Velocidade vertical por diferença finita da pressão-altitude (issue 04):
    // (alt_agora − alt_antes)/Δt, entre leituras FRESCAS de baro, não a cada ciclo.
    // Guarda o instante da última leitura fresca para o Δt real, e a última
    // velocidade calculada, que é mantida nos subciclos sem baro — como a altitude.
    // Diferença finita crua: sem integração inercial, sem estado de filtro que
    // possa divergir.
    uint32_t last_baro_ms_ = 0;
    int16_t  last_vertical_speed_dms_ = 0;
};

}  // namespace core
