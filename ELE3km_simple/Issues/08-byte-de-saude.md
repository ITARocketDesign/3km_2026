# 08 — Byte de saúde honesto

**Tipo:** Bancada
**User stories:** 16, 50

## What to build

Preenche o byte de saúde (byte 18 do pacote, e o campo correspondente do registro)
com o estado real de cada subsistema, para a estação diagnosticar do chão.

Bitmap, bit em 1 = subsistema OK, usando as constantes de `health.h` copiadas:

- bit 0 — IMU (já produziu leitura válida e não está ausente)
- bit 1 — barômetro
- bit 2 — GPS (receptor vivo, "está falando", não "tem fix")
- bit 3 — microSD
- bit 4 — E22: **reservado, sempre 0** (o E22 foi abandonado; mantém o formato)
- bit 5 — SX1276
- bit 6 — **altitude é barométrica (1)** ou caiu para a altitude do GPS por baro
  ausente (0). No fallback não há referência reusada; o bit reflete só "tem baro".

No superloop os bits vêm todos do mesmo lugar (uma thread) — não há publicação
entre tasks como no ELE3km.

## Acceptance criteria

- [ ] Byte de saúde preenchido com os bits reais de IMU/baro/GPS-vivo/SD/SX1276
- [ ] Bit 4 (E22) sempre 0
- [ ] Bit 6 = 1 quando a altitude é barométrica; 0 quando cai para GPS por baro ausente
- [ ] Bit 2 (GPS) reflete "receptor vivo", não "tem fix" (consistente com a issue 05)
- [ ] O mesmo bitmap vai ao pacote e ao registro de log
- [ ] Teste nativo: cada condição de subsistema liga/desliga o bit certo; baro ausente
      apaga bit 1 e bit 6 juntos
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: desconectar um módulo apaga o bit correspondente no pacote recebido
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 05 (GPS vivo/fix), 06 (SD)

## Estado da implementação

**Fechada em 2026-09-03.** O núcleo compõe o byte de saúde honesto; `pio test -e
native` passa com 31 casos (26 antes + 5 novos de saúde), `pio run` compila (Flash
28,0 %) e as cinco greps de `DISCIPLINE.md` saem vazias. O formato congelado não
mudou: o campo `health` já existia no `TelemetryPacket` (byte 18) e no `LogRecord`;
esta fatia só o **preenche**. Nenhuma mudança em `telemetry_codec` nem `log_codec`.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/survival_computer.cpp` (`update`) | Monta `packet_health` (bits 0–6) e `record_health` (= packet + bit 7). Fontes: bit 0 `sample.imu_valid`, bit 2 `gps.receiving` (da amostra); bits 1/6 `io.baro`, bit 3 `io.sd`, bit 5 `io.sx1276` (de `io`); bit 4 (E22) nunca setado; bit 7 (kAccelSat) só no registro (pré-existente). |
| `src/core/survival_computer.h` | `update()` ganha um parâmetro `const IoSubsystemHealth& io = {}` (trailing, com default tudo-ausente). |
| `src/core/types.h` (`IoSubsystemHealth`) | Ganha `bool baro`; comentário reescrito: a struct carrega a saúde que o núcleo não lê estável pela amostra (cartão, rádio e baro-vivo). |
| `src/main.cpp` (`loop`) | Monta `io` das flags da HAL — `io.baro = g_baro_ok` (issue 07), `io.sd = g_log.is_open()`, `io.sx1276 = g_sx1276_ok` — e passa ao `update()`. |
| `test/test_survival_computer/…` | +5 casos (IMU, baro-sem-piscar, SD/SX1276, E22=0, bitmap idêntico pacote↔registro); 2 casos de bit 6 atualizados para passar `io.baro`. |

**A decisão de projeto (autorizada antes de codar): bits 1 e 6 vêm de `io.baro`,
não de `sample.baro_valid`.** O baro é lido a 25 Hz dentro do laço de 50 Hz, então
`sample.baro_valid` é falso em metade dos ciclos mesmo com o sensor são — dirigir o
bit por ele faria a saúde do baro piscar quase aleatoriamente no pacote de 1 Hz. A
verdade estável "baro vivo" é a flag `g_baro_ok` da HAL, que a issue 07 já apaga
quando o `begin()` pós-recuperação falha. Assim o bit é estável entre subciclos **e**
apaga honestamente quando o baro morre em voo. Sem GPS-altitude reusada no fallback,
"altitude é barométrica" (bit 6) é o mesmo sinal de "baro vivo" (bit 1) — apagam
juntos. imu e gps são lidos a cada volta, então seguem vindo da amostra sem piscar.

O `io` entrou como parâmetro **trailing com default `{}`** (tudo-ausente, o caso
honesto): só o chamador de produção (`main.cpp`) o passa explícito; os ~25 testes que
não exercem esses bits seguem no default sem alteração.

**Fila de bancada** (a AC de target, fora do `/tdd`): desconectar um módulo apaga o
bit correspondente no pacote recebido pela estação de solo. Precisa de placa +
receptor de 915 MHz + os módulos. ⚠️ Só ligar com a antena do SX1276 conectada.
