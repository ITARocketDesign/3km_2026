// hal/i2c_bus.h — recuperação do barramento I²C travado (issue 07).
//
// A defesa contra o modo de falha mais provável da placa (hazard H5): o GND da
// IMU flutua nesta revisão, e uma peça nesse estado não "deixa de responder"
// limpo — ela segura a linha de dados baixa e trava o barramento inteiro,
// levando o barômetro junto. Nenhum retry por software resolve enquanto a linha
// estiver presa: só bordas de clock soltam um escravo preso no meio de um byte.
//
// Esta é só o MECANISMO. Quem chama esta rotina — numa leitura I²C que falha, e no
// máximo um módulo por ciclo — é o superloop (issue 07). O fallback NÃO tem a
// política de diagnóstico do ELE3km: sem retentativa periódica de 5 s, sem máquina
// de saúde {OK, DEGRADED, FAILED}, sem contador de reinit. Se o begin() pós
// recuperação falhar, a peça sumiu, a flag do módulo latcheia falsa e ele para de
// ser lido; o backstop é o watchdog/reboot da issue 02.
#pragma once

namespace hal {

// A rotina de recuperação da mitigação #1 do H5, na ordem exata: solta a linha de
// dados, pulsa o clock ~9 vezes para clocar para fora um escravo travado, emite
// uma condição de STOP, e reinicializa o driver a 100 kHz (H9 — nunca 400 kHz).
//
// Faz bit-bang direto nos pinos de SDA/SCL do netlist, então TEM que rodar com o
// driver do barramento solto; ela mesma o solta e o reergue. Depois dela, o
// chamador re-roda o begin() do dispositivo afetado para reaplicar timeout e
// configuração. Custa ~10–50 ms (um estouro de ciclo, nunca sustentado): é a
// janela que o buffer de UART de 512 B do GPS (issue 02) existe para absorver.
void i2c_bus_recover();

}  // namespace hal
