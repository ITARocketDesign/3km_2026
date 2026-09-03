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

## Estado da implementação (2026-09-02)

Feito e coberto por teste nativo. Falta só a verificação de bancada (receptor
ouvindo a 1 Hz), que espera o hardware.

- **Núcleo novo `core/survival_computer.{h,cpp}`** — o análogo enxuto do
  `FlightComputer::update`, por TDD (7 testes em `test/test_survival_computer/`).
  `update(sample, t_ms)` devolve `Outputs { LogRecord record; TelemetryPacket
  packet; bool has_packet; }`. Sem estado global, sem relógio, sem header de
  Arduino — o `native` (que compila só `src/core` sem Arduino) é a prova disso.
  - **Altitude:** `altitude_from_pressure(pressao, kFixedDatumPa)` com
    `kFixedDatumPa = 101325.0f` definido neste header (o datum ISA fixo).
  - **Mantida entre ciclos:** o baro é lido a 25 Hz dentro do laço de 50 Hz, então
    a última altitude derivada é guardada — um pacote nunca sai com 0 por cair num
    subciclo sem leitura fresca. `have_baro_altitude_` é o bit 6 (`kAltRef`).
  - **Baro ausente:** altitude 0 e bit 6 apagado (o fallback para altitude de GPS é
    da issue 05).
  - **Pacote só-altitude (12 B):** fase = `Flight` (=1), fonte = `None` (=0),
    `t_ds` = decissegundos desde o boot (satura), sequência u16 incrementa por
    pacote.
  - **Registro:** montado todo ciclo, espelho do cru + altitude derivada; sequência
    u32 por ciclo. Campos das issues 04/07/08 ficam em zero.
- **Cadência de TX no núcleo, baseada no tempo** (decisão do grill deste ciclo):
  `has_packet` sobe quando `t_ms` avançou ≥ `kTxPeriodMs` (1000 ms) desde o último
  pacote; o primeiro ciclo já emite (telemetria do power-on). Robusto ao jitter do
  laço e testável no `native` — a alternativa (contar 50 voltas no `main`) não é
  nem uma coisa nem outra.
- **Byte de saúde:** nesta fatia só o bit 6. O bitmap honesto completo (IMU, baro,
  GPS-vivo, SD, SX1276) continua na issue 08.
- **`main.cpp`:** o superloop chama `update()` todo ciclo, faz `g_sx1276.service()`
  a cada volta e dispara `g_sx1276.start_send()` quando `has_packet`. Sequencial —
  uma thread só, sem regra de arbitragem de barramento. Rádio em SF7 (o `begin()`
  da issue 02 já sobe em modo Flight). O diagnóstico do Serial agora mostra a
  altitude derivada e o pacote transmitido.

### Definition of Done

1. ✅ `pio test -e native` — 12/12 (5 de contrato + 7 do SurvivalComputer).
2. ✅ `pio run -e heltec_wifi_lora_32_V2` compila.
3. ✅ As cinco greps de `DISCIPLINE.md` saem vazias.
4. ✅ Esta seção.
5. ✅ `NEXT.md` atualizado (03 → ✅, ▶ move para a 04).

### Aberto para a bancada (precisa da placa e do receptor)

- O `3km913hzReceiver` **sem alteração** decodifica altitude coerente com a local,
  a 1 Hz.
- ⚠️ O `radio_sx1276.cpp` copiado ainda está em **+2 dBm** (potência de bancada
  reduzida). A TX de voo é +20 dBm — reverter antes do teste de alcance, e nunca
  transmitir sem antena.
