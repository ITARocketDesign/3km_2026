// core/telemetry_codec.h — layout binário dos pacotes de rádio.
//
// ESTE ARQUIVO É O CONTRATO COM A ESTAÇÃO DE SOLO. Os dois rádios enviam o
// mesmo formato e o mesmo conteúdo (PRD §6), então a estação trata os dois
// receptores de forma idêntica. Qualquer mudança aqui quebra o solo.
//
// Tudo little-endian. Sem CRC de aplicação: o LoRa já tem CRC de hardware e o
// driver não entrega pacote reprovado. No cartão o CRC permanece, porque lá não
// existe camada de enlace.
//
// ── Pacote completo — 20 B ──────────────────────────────────────────────────
//
//   Off  Tipo  Campo
//    0   u8    magic (4 bits altos) + versão (4 bits baixos)
//    1   u16   sequência — global, compartilhada pelos dois rádios
//    3   u16   tempo, em decissegundos desde o liftoff
//    5   i32   latitude × 1e7
//    9   i32   longitude × 1e7
//   13   i16   altitude, metros acima da referência
//   15   i16   velocidade vertical, dm/s
//   17   u8    flags
//   18   u8    saúde: bitmap {imu, baro, gps, sd, e22, sx1276}
//   19   u8    GPS: satélites (4 bits baixos) + HDOP×2 (4 bits altos)
//
// ── Pacote só-altitude — 12 B ──────────────────────────────────────────────
//
// Idêntico, sem latitude e longitude. Altitude está presente nas DUAS formas.
//
//   Off  Tipo  Campo
//    0   u8    magic + versão
//    1   u16   sequência
//    3   u16   tempo em decissegundos
//    5   i16   altitude, metros acima da referência
//    7   i16   velocidade vertical, dm/s
//    9   u8    flags
//   10   u8    saúde
//   11   u8    GPS: satélites + HDOP×2
//
// As duas formas se distinguem pelo COMPRIMENTO do payload, que o cabeçalho
// explícito do LoRa entrega ao receptor. Não há campo de tipo.
//
// ── Por que 20 B, e não 22 ──────────────────────────────────────────────────
//
// Airtime LoRa é quantizada em blocos de 8 símbolos. No fator de espalhamento do
// voo isso são ~16 ms por degrau, e o teto do bucket-alvo é 21 B. Um pacote de
// 22 B pula um degrau e fura o teto de ciclo de trabalho já dimensionado nos
// docs de hardware. O tamanho não é folga de projeto: é orçamento gasto.
//
// ── Byte de flags ───────────────────────────────────────────────────────────
//
//   bits 0–2  fase de voo      (FlightPhase)
//   bits 3–4  fonte de posição (PositionSource)
//   bits 5–7  qualidade do fix (0–7) — em POUSADO, nº de amostras pós-pouso (0–7),
//             desambiguado pela fase (issue 13). O codec serializa os 3 bits;
//             quem escolhe o sentido é o FlightComputer.
//
// ── Byte de GPS ─────────────────────────────────────────────────────────────
//
//   bits 0–3  satélites, saturando em 15
//   bits 4–7  HDOP × 2, saturando em 15 → 0,0 a 7,5 em passos de 0,5
//
// ── Saturação do campo de tempo ─────────────────────────────────────────────
//
// 65535 ds = ~109 min depois do liftoff. Cobre o voo inteiro e ~107 min de
// beacon. Depois disso a estação de solo ordena e deduplica pelo número de
// sequência (u16 = ~18 h a 1 Hz).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

namespace core {

constexpr size_t kAltitudePacketSize = 12;
constexpr size_t kFullPacketSize     = 20;

constexpr uint8_t kPacketMagic   = 0xE;  // 4 bits
constexpr uint8_t kPacketVersion = 0x1;  // 4 bits

// Capacidade que serve às duas formas.
constexpr size_t kMaxPacketSize = kFullPacketSize;

// Serializa `packet` na forma que ele declara em `packet.form`. Devolve o número
// de bytes escritos, ou 0 se capacity não couber essa forma.
size_t encode_packet(const TelemetryPacket& packet, uint8_t* out, size_t capacity);

// Desserializa. A forma vem do COMPRIMENTO, que o cabeçalho explícito do LoRa
// entrega ao receptor; qualquer outro comprimento é rejeitado, assim como
// magic/versão que não batam.
//
// Existe para os testes e para o harness de replay (issue 14) — o firmware de
// voo não decodifica nada, ele só transmite.
bool decode_packet(const uint8_t* in, size_t len, TelemetryPacket& out);

}  // namespace core
