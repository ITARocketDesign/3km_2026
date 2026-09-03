// hal/board.h — estado seguro dos pinos no boot.
#pragma once

namespace hal {

// A PRIMEIRA coisa que setup() faz, antes de SPI.begin() e antes de qualquer
// driver. Coloca os três chip-selects em nível alto e desliga WiFi e Bluetooth.
//
// Hazards H2 e H14: nenhum dos três chip-selects tem pull-up, e o ESP32 os deixa
// em alta impedância nas primeiras centenas de ms do bootloader. Se GPIO18
// flutuar para baixo, o SX1276 se auto-seleciona e dirige o MISO contra o cartão
// e o E22 — corrupção aleatória de SPI, do tipo que se diagnostica errado por
// dias.
void board_early_init();

}  // namespace hal
