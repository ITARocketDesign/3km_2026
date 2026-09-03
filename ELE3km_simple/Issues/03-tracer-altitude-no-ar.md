# 03 — Tracer bullet: altitude no ar (pressão-altitude, datum fixo)

**Tipo:** Bancada
**User stories:** 2, 11, 46, 50

## What to build

A primeira fatia vertical completa do fallback: a placa liga, lê pressão, deriva a
**pressão-altitude contra o datum ISA fixo (101325 Pa)**, monta o pacote só-altitude
de 12 B e um receptor em 915 MHz ouve a 1 Hz. Corta build, HAL, codec e rádio no
caminho mais fino.

A montagem do pacote e do registro vive numa função pura de núcleo, o análogo enxuto
do `FlightComputer::update` do ELE3km — recebe a amostra e o tempo, devolve o
registro e o pacote. **Sem filtro, sem fase, sem escalonador.** Nesta fatia:

- **Altitude:** `altitude_from_pressure(pressao, 101325.0f)` — o datum fixo. Se o
  baro está ausente, cai para a altitude do GPS (quando houver, na issue 05) e o bit
  6 de saúde apaga; sem GPS, altitude 0 e bit 6 apagado.
- **Cadência de TX:** a cada 1 s (a cada 50 voltas do laço), monta o pacote, incrementa
  a **sequência** (u16 global), transmite pelo SX1276 em **SF7**. TX não-bloqueante
  com IRQ de fim, borda confirmada contra o registrador de status (padrão do driver
  copiado).
- **Campos fixados do pacote:** bits de fase = `1` (em voo); fonte de posição = `0`
  (nenhuma) nesta fatia; tempo = decissegundos **desde o boot**.
- **Datum no cabeçalho do log:** o campo de referência barométrica do cabeçalho de 64 B
  recebe `101325.0f`, para a análise pós-voo saber contra qual zero a altitude foi
  derivada.

Regra de arbitragem SPI: **não existe.** O laço é sequencial — a escrita no cartão
(issue 06) e a TX nunca acontecem ao mesmo tempo porque é uma thread só.

## Acceptance criteria

- [ ] Função de núcleo pura monta `{ LogRecord, TelemetryPacket }` a partir de
      `SensorSample` + `t_ms`, sem estado global e sem header Arduino
- [ ] Altitude = pressão-altitude contra 101325 Pa (datum fixo)
- [ ] Baro ausente → altitude do GPS (se houver) e bit 6 de saúde apagado
- [ ] Pacote só-altitude de **exatamente 12 B**, bits de fase = 1, fonte = 0
- [ ] Sequência u16 incrementa por pacote; tempo em ds desde o boot
- [ ] Campo de referência barométrica do cabeçalho de log = 101325 Pa
- [ ] TX a 1 Hz pelo SX1276 em SF7, não-bloqueante, sem espera indefinida no busy
- [ ] Teste nativo: altitude aparece em todo pacote; datum fixo produz a
      pressão-altitude esperada para pressões conhecidas
- [ ] Teste nativo: baro ausente apaga o bit 6 de saúde
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: o `3km913hzReceiver` **sem alteração** decodifica altitude coerente
      com a altitude local, a 1 Hz
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 02 (superloop, HAL, watchdog)
