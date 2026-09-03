// hal/radio_sx1276.h — adaptador do SX1276 onboard do Heltec (915–928 MHz).
//
// O único rádio da placa: vive no regulador interno do Heltec (rail de 5 V) e
// transmite 100 mW. O E22 de 433 MHz que existia no projeto foi removido
// (Docs/ELE3km_drop_e22_single_radio.md); todo o link de telemetria e o beacon de
// recuperação passam por este módulo.
//
// Faixa: 915–928 MHz, a faixa livre nacional. NÃO 868 MHz, que é o default de
// metade dos exemplos de biblioteca e não é livre no Brasil (PRD §5).
//
// Transmissão não-bloqueante: a carga útil é escrita no rádio, a transmissão é
// disparada, e o airtime corre sozinho. Duas regras de §12 valem aqui:
//
//   • a borda de IRQ NUNCA é acreditada sozinha — ela é sempre confirmada
//     contra o registrador de status do rádio;
//   • nunca se espera indefinidamente pelo fim de uma transmissão — há timeout
//     limitado e caminho de recuperação por reset.
//
// ⚠️ Nunca transmitir sem antena conectada. O firmware não tem como detectar
// isso; é regra de operação.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {

enum class RadioSx1276Mode : uint8_t {
    Flight,
    SurvivalBeacon,
};

class RadioSx1276 {
  public:
    // Configura o rádio e arma a interrupção de fim de transmissão.
    bool begin(RadioSx1276Mode mode = RadioSx1276Mode::Flight);

    // true quando não há transmissão em andamento e o rádio está utilizável.
    bool is_ready() const { return initialized_ && !tx_in_progress_; }

    // Escreve a carga útil e dispara a transmissão. Retorna imediatamente: o
    // airtime corre sem bloquear ninguém. Devolve false se o rádio não estiver
    // pronto ou se a biblioteca recusar o disparo.
    bool start_send(const uint8_t* data, uint8_t len, uint32_t now_ms);

    // Chamada a cada volta do laço. Confirma o fim da transmissão contra o
    // registrador de status e aplica o timeout.
    void service(uint32_t now_ms);

    // Diagnóstico: quantas transmissões estouraram o timeout e forçaram reset.
    uint32_t timeout_count() const { return timeout_count_; }

  private:
    void finish_tx();
    void recover();

    bool     initialized_ = false;
    bool     tx_in_progress_ = false;
    uint32_t tx_deadline_ms_ = 0;
    uint32_t timeout_count_ = 0;
    RadioSx1276Mode mode_ = RadioSx1276Mode::Flight;
};

}  // namespace hal
