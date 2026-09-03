// hal/gps_neo6m.h — adaptador do NEO-6M (UART, NMEA + configuração UBX).
//
// A configuração é o núcleo deste adaptador, não um detalhe de inicialização.
// O NEO-6M sai de fábrica no modelo dinâmico *Portable*, que assume menos de
// 1 g. Sob os 10–20 g do boost o receptor REJEITA A PRÓPRIA SOLUÇÃO e derruba o
// fix; a reaquisição leva 10–60 s e a equipe perde a subida inteira. Pior: uma
// queda de rail devolve o receptor para *Portable* silenciosamente (hazard H3),
// então configurar uma vez no boot não basta.
//
// A barra de sobrevivência (issue 05) resolve isso da forma mais burra possível:
//
//   1. Configura no boot: modelo Airborne <4g, sentenças inúteis desligadas,
//      taxa em 5 Hz. Sem salvar na memória com bateria de backup — a barra não
//      depende de a célula do breakout estar viva.
//   2. Reenvia a configuração INTEIRA, incondicionalmente, a cada ~10 s, para
//      sempre. Um receptor que resetou silenciosamente (voltando a Portable) é
//      reconfigurado em ≤10 s. NÃO há detecção de reset: nenhuma heurística de
//      sentença-que-devia-estar-desligada, nenhuma de satélites-a-zero-após-TX,
//      nenhum modo Stationary no pouso. A config é idempotente; reenviá-la quando
//      nada resetou é inofensivo, e é justamente por não haver estado de detecção
//      que não há lugar para o adaptador travar no estado errado.
//   3. Conta overflows do buffer de recepção, que de outra forma perderiam
//      sentenças em silêncio.
//
// Não há espera por ACK do receptor. Uma config que não pegou é reenviada no
// próximo tique de ~10 s de qualquer forma; esperar ACK só acrescentaria uma
// espera bloqueante no meio do voo.
//
// Só a GGA sobrevive à configuração, e ela carrega exatamente os campos do
// pacote: posição, qualidade do fix, satélites e HDOP. A interpretação das
// sentenças é do núcleo puro (core/nmea.h), onde a suíte nativa a exercita
// contra sentenças conhecidas; aqui fica o que depende de hardware e de tempo.
#pragma once

#include <stdint.h>

#include "core/nmea.h"
#include "core/types.h"

class HardwareSerial;

namespace hal {

// Tamanho do buffer de recepção, em bytes. O default do ESP32 é 256, e o GPS a
// 5 Hz gera centenas de bytes por segundo de NMEA: uma ocupação temporária da
// task de aquisição transborda o buffer e perde sentenças em silêncio. A 9600
// baud, 512 B dão ~530 ms de folga — mais que qualquer recuperação de I²C.
constexpr uint16_t kGpsRxBufferBytes = 512;

class GpsNeo6m {
  public:
    explicit GpsNeo6m(HardwareSerial& uart) : uart_(uart) {}

    // Abre a UART com o buffer dimensionado e envia a configuração inicial.
    // Devolve false se a UART não aceitar o buffer pedido — nesse caso o
    // adaptador não é usado, porque perder sentenças em silêncio é pior que
    // não ter GPS.
    bool begin(uint32_t now_ms);

    // Drena a UART, atualiza o fix e, a cada ~10 s, reenvia a configuração
    // inteira. Precisa ser chamada com folga sobre os 530 ms de fôlego do buffer.
    void service(uint32_t now_ms);

    // O melhor fix conhecido em now_ms. Um fix parado há mais que a janela de
    // frescor deixa de ser válido: ele ainda é a última posição conhecida, mas
    // isso é decisão da issue 08, não deste adaptador.
    core::GpsFix fix(uint32_t now_ms) const;

    // Diagnóstico, para o log. Nenhum dos dois pede ação: a sentença perdida já
    // foi perdida, e a reconfiguração periódica já acontece sozinha. O que eles
    // dizem é para a próxima revisão — overflows subindo acusam a drenagem. O
    // contador de reconfigurações aqui é só a cadência burra de ~10 s (sobe
    // monotonicamente), não um sinal de reset detectado.
    uint32_t uart_overflow_count() const { return uart_overflow_count_; }
    uint32_t reconfigure_count() const { return reconfigure_count_; }

  private:
    void apply_configuration(uint32_t now_ms);
    void send_ubx(uint8_t message_class, uint8_t message_id, const uint8_t* payload,
                  uint8_t length);
    void drain(uint32_t now_ms);
    void accept_gga(const core::GgaReading& reading, uint32_t now_ms);

    HardwareSerial&   uart_;
    core::NmeaParser  parser_;

    core::GpsFix fix_;
    uint32_t     last_fix_ms_ = 0;
    uint32_t     last_sentence_ms_ = 0;
    bool         has_fix_ = false;
    bool         has_sentence_ = false;

    uint32_t last_reconfigure_ms_ = 0;
    uint32_t reconfigure_count_ = 0;
    uint32_t uart_overflow_count_ = 0;
};

}  // namespace hal
