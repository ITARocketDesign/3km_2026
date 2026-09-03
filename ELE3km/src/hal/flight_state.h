// hal/flight_state.h — persistência em NVS da fase e da referência (issue 06).
//
// A máquina de fases decide QUANDO persistir e O QUE persistir; esta HAL só grava
// e lê os bytes, e responde uma pergunta que só a placa sabe: o último reset foi
// um power-on limpo? A DECISÃO de reusar a referência salva é do núcleo
// (core/flight_phase.h), e depende dessas duas coisas.
//
// São poucas escritas por voo — liftoff e cada transição de fase, ~5 no total —
// porque gravar em NVS a 100 Hz destruiria o flash em minutos. A gravação bloqueia
// alguns milissegundos, então quem a chama é a task io (core 0), nunca a task de
// voo a 100 Hz.
#pragma once

#include "core/flight_phase.h"

namespace hal {

class FlightState {
  public:
    // Lê o snapshot salvo (se houver) e o motivo do reset, e monta o contexto de
    // restauração que a máquina de fases consome no boot.
    core::PhaseRestore restore();

    // Grava o snapshot em NVS. Bloqueante; chamar só da task io.
    void save(const core::PhaseSnapshot& snapshot);
};

}  // namespace hal
