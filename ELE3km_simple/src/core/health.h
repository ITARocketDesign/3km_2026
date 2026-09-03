// core/health.h — a máquina de saúde uniforme sobre todos os drivers (issue 10).
//
// Três estados iguais em todo subsistema: {OK, DEGRADED, FAILED}. Esta classe é
// a POLÍTICA de saúde — quando retentar, quando reverificar a configuração, e
// quanto o sensor já reinicializou. Ela não faz I/O: o chamador (a HAL) executa
// os begin()/read() reais do driver e reporta o resultado aqui; a máquina decide
// o próximo passo. É o mesmo corte do resto do core/: o tempo entra como
// parâmetro, nada global, nada dinâmico.
//
// Por que período fixo de 5 s, e não backoff: uma falha transitória no lift-off
// não pode custar o sensor pelo voo inteiro. Um backoff com teto de 5 s chegaria
// ao teto em três tentativas num voo de ~200 s — a mesma coisa, com mais estado
// para testar. Uma cadência única (retentativa E reverificação) mantém o mínimo
// de estado.
#pragma once

#include <stdint.h>

namespace core {

// Cadência única da máquina: um módulo não-OK é retentado a cada 5 s, e o caminho
// saudável relê os registradores de configuração na mesma cadência.
constexpr uint32_t kHealthRetryPeriodMs = 5000;

enum class HealthState : uint8_t {
    Ok       = 0,
    Degraded = 1,  // responde, mas voltou com a configuração errada (brown-out)
    Failed   = 2,  // não responde — morto ou barramento travado
};

class SubsystemHealth {
  public:
    SubsystemHealth() = default;

    // Resultado da detecção de startup. Presente → OK; ausente → FAILED, com a
    // primeira retentativa agendada para now_ms + 5 s.
    void begin(bool present, uint32_t now_ms);

    // Resultado de uma operação de dados (a leitura real do driver, já com o
    // timeout duro aplicado) no caminho saudável. Uma leitura que falha depois do
    // timeout é um sensor morto ou um barramento travado: manda o módulo para
    // FAILED e agenda a retentativa. Uma leitura boa não mexe no estado — não
    // limpa, sozinha, uma configuração errada já detectada (ver report_config).
    void report_operation(bool ok, uint32_t now_ms);

    // True quando um módulo não-OK está na hora de ser retentado (>= 5 s desde a
    // última tentativa). O chamador re-roda o begin() do driver e reporta o
    // resultado em observe_recovery(). Sempre false num módulo OK.
    bool recovery_due(uint32_t now_ms) const;

    // Resultado da retentativa (o begin() real do driver). Sucesso → OK e o
    // contador de reinicializações sobe. Falha → permanece no estado não-OK atual,
    // próxima tentativa em +5 s.
    void observe_recovery(bool ok, uint32_t now_ms);

    // True quando o caminho saudável deve reler os registradores de CONFIGURAÇÃO
    // (>= 5 s desde a última verificação). Só num módulo OK — é a defesa contra o
    // sensor que voltou com a configuração de fábrica e devolve dados plausíveis
    // com a escala errada, que nenhuma leitura de dados detecta.
    bool config_verify_due(uint32_t now_ms) const;

    // Resultado da reverificação de configuração. Bate → segue OK e reagenda a
    // próxima janela. Não bate → configuração de fábrica de um brown-out: DEGRADED,
    // com a reinicialização (que reaplica a configuração) agendada.
    void report_config(bool matches, uint32_t now_ms);

    HealthState state() const { return state_; }
    bool        ok() const { return state_ == HealthState::Ok; }
    uint16_t    reinit_count() const { return reinit_count_; }

  private:
    HealthState state_ = HealthState::Failed;
    uint16_t    reinit_count_ = 0;
    uint32_t    next_recovery_ms_ = 0;  // válido quando state_ != Ok
    uint32_t    last_verify_ms_ = 0;    // válido quando state_ == Ok
};

}  // namespace core
