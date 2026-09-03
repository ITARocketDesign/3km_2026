// ELE3km_simple — firmware de último recurso.
//
// Esqueleto mínimo (issue 01): existe só para o env de target compilar. O
// superloop de 50 Hz, o bring-up da placa e o watchdog entram na issue 02, e a
// HAL (com RadioLib e SdFat) junto. Ver DISCIPLINE.md regra 1 antes de tocar no
// pino indicador do módulo (GPIO25): ele é o habilitador de transmissão.
#include <Arduino.h>

void setup() {}

void loop() {}
