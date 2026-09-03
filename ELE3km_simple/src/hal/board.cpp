#include "hal/board.h"

#include <Arduino.h>
#include <WiFi.h>

#include "pins.h"

namespace hal {

void board_early_init() {
    // Ordem obrigatória, toda ela antes de SPI.begin().
    pinMode(PIN_LORA_CS, OUTPUT);
    digitalWrite(PIN_LORA_CS, HIGH);  // desseleciona o SX1276 onboard
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);  // desseleciona o cartão
    pinMode(PIN_E22_NSS, OUTPUT);
    digitalWrite(PIN_E22_NSS, HIGH);  // desseleciona o E22

    // O E22 foi removido do projeto (Docs/ELE3km_drop_e22_single_radio.md), mas o
    // chip continua soldado na placa e alimentado. Nenhum driver sobe para ele, e a
    // regra de segurança passa a valer PERMANENTEMENTE: NSS fica alto (fora do
    // barramento, sem disputar o MISO) e NRST fica baixo (o módulo segue em reset,
    // saídas em alta impedância). O pino de NRST não tem pull-up externo (R3 não
    // montado), então PRECISA ser dirigido — solto, o módulo sairia do reset em
    // estado indefinido sem ninguém para configurá-lo.
    pinMode(PIN_E22_NRST, OUTPUT);
    digitalWrite(PIN_E22_NRST, LOW);

    // Nunca se toca em GPIO12 (strapping, hazard H7) nem em GPIO25 (o habilitador de
    // transmissão do PA de 1 W, hazard H8). Sem o driver do E22, ninguém os assume:
    // ficam presos em baixo pelos pulldowns R1/R2 da placa, o que mantém o PA
    // desligado — exatamente o estado seguro. GPIO14 é o reset do SX1276 e pertence
    // ao driver dele.

    // Nenhum dos dois é usado, e ambos custam corrente em rajadas e interrupções
    // de CPU (PRD §2, user story 70).
    WiFi.mode(WIFI_OFF);
    btStop();
}

}  // namespace hal
