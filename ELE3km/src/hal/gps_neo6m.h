// hal/gps_neo6m.h — adaptador do NEO-6M (UART, NMEA + configuração UBX).
//
// A configuração é o núcleo deste adaptador, não um detalhe de inicialização.
// O NEO-6M sai de fábrica no modelo dinâmico *Portable*, que assume menos de
// 1 g. Sob os 10–20 g do boost o receptor REJEITA A PRÓPRIA SOLUÇÃO e derruba o
// fix; a reaquisição leva 10–60 s e a equipe perde a subida inteira. Pior: uma
// queda de rail devolve o receptor para *Portable* silenciosamente (hazard H3),
// então configurar uma vez no boot não basta.
//
// Por isso o adaptador faz três coisas que um wrapper de UART não faria:
//
//   1. Configura no boot: modelo Airborne <4g, sentenças inúteis desligadas,
//      taxa em 5 Hz, e a configuração salva na memória com bateria de backup
//      para sobreviver a um reset por queda de rail.
//   2. Vigia o fluxo em busca dos dois sinais de que o receptor resetou, e
//      reaplica a configuração INTEIRA quando vê um deles.
//   3. Conta overflows do buffer de recepção, que de outra forma perderiam
//      sentenças em silêncio.
//
// Não há espera por ACK do receptor, nem no boot nem na reaplicação. A razão é
// que a detecção de reset já É a verificação: se a configuração não pegou, as
// sentenças que mandamos desligar continuam chegando, o detector dispara de
// novo, e a configuração é reenviada. Esperar ACK acrescentaria uma espera
// bloqueante no meio do voo para chegar à mesma conclusão mais devagar.
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

    // Drena a UART, atualiza o fix e vigia os sinais de reset do receptor.
    // Precisa ser chamada com folga sobre os 530 ms de fôlego do buffer.
    void service(uint32_t now_ms);

    // O melhor fix conhecido em now_ms. Um fix parado há mais que a janela de
    // frescor deixa de ser válido: ele ainda é a última posição conhecida, mas
    // isso é decisão da issue 08, não deste adaptador.
    core::GpsFix fix(uint32_t now_ms) const;

    // Chamada quando um rádio transmite. Alimenta o segundo detector de reset:
    // satélites caindo a zero logo depois de uma transmissão é a assinatura de
    // um brown-out do receptor causado pelo pico de corrente do PA.
    void note_transmission(uint32_t now_ms) { last_tx_ms_ = now_ms; has_tx_ = true; }

    // Diagnóstico, para o log. Nenhum dos dois pede ação: a sentença perdida já
    // foi perdida, e a reconfiguração já aconteceu. O que eles dizem é para a
    // próxima revisão — overflows subindo acusam a drenagem, reconfigurações
    // subindo acusam o rail.
    uint32_t uart_overflow_count() const { return uart_overflow_count_; }
    uint32_t reconfigure_count() const { return reconfigure_count_; }

  private:
    void apply_configuration(uint32_t now_ms);
    void request_reconfigure(uint32_t now_ms);
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

    uint32_t last_tx_ms_ = 0;
    bool     has_tx_ = false;

    uint32_t last_reconfigure_ms_ = 0;
    uint32_t reconfigure_count_ = 0;
    uint32_t uart_overflow_count_ = 0;
};

}  // namespace hal
