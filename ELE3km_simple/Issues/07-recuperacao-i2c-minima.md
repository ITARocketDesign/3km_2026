# 07 — Recuperação mínima do barramento I²C

**Tipo:** Bancada
**User stories:** guarda do payload prioritário (altitude vem do baro por I²C)

## What to build

A única recuperação de barramento que sobrevive à simplificação, porque a altitude —
o payload prioritário — depende do baro no I²C.

- **No read que falha** (baro ou IMU) depois do timeout duro do adaptador: um
  barramento travado é um escravo segurando SDA em nível baixo, e um `begin()` sozinho
  não resolve. Roda o **clock-out por bit-bang** (`i2c_bus_recover` copiado do ELE3km)
  para soltar a linha, e reinicializa o driver.
- **No máximo um módulo recuperado por ciclo** — a recuperação custa ~10–50 ms e não
  pode comer o ciclo de forma sustentada. Se o `begin()` falhar, o módulo fica ausente
  e para de ser lido (o watchdog/reboot é o backstop).
- **Sem** contador de reinit por sensor, **sem** retentativa periódica de 5 s, **sem**
  bitmap durável de degradação — eram diagnóstico da issue 10 do ELE3km, não
  sobrevivência.

## Acceptance criteria

- [ ] Read de baro/IMU que falha dispara clock-out + `begin()`, no máximo 1 por ciclo
- [ ] `i2c_bus_recover` copiado do ELE3km, sem número de GPIO literal fora de `pins.h`
- [ ] Módulo cujo `begin()` falha fica ausente e para de ser lido no ciclo
- [ ] Sem contador de reinit, sem retentativa de 5 s (verificável por revisão)
- [ ] `pio run` compila; o núcleo puro segue sem header Arduino
- [ ] No target: soltar/reconectar o baro no barramento provoca uma recuperação e a
      altitude volta sem reset da placa
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 03 (leitura de baro no laço)

## Estado da implementação

**Fechada em 2026-09-03 (fatia HAL/target — sem teste nativo novo).** Igual à issue
09 do ELE3km: a recuperação de barramento é ação física sobre os pinos e mora
inteira na HAL, então nenhuma linha de `src/core/` mudou e o env `native` (que só
compila `src/core/`) segue com os mesmos 26 casos passando. `pio run` compila (Flash
28,0 %) e as cinco greps de `DISCIPLINE.md` saem vazias.

O mecanismo já vinha copiado do ELE3km; esta fatia acrescentou só a **política de
chamada** no superloop.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/hal/i2c_bus.{h,cpp}` | Já presente — `i2c_bus_recover()` copiado do ELE3km: solta SDA, ~9 pulsos de SCL, STOP, `Wire.begin()` a 100 kHz. Sem GPIO literal (usa `PIN_I2C_SDA`/`PIN_I2C_SCL` de `pins.h`). **Não tocado nesta fatia.** |
| `src/main.cpp` (`loop`) | **Novo.** Numa leitura I²C que falha, chama `hal::i2c_bus_recover()` + `begin()` do dispositivo; guarda de um módulo recuperado por ciclo (`i2c_recovered`); IMU antes do baro (ordem de leitura do laço). Se `begin()` falhar, `g_imu_ok`/`g_baro_ok` latcheiam falsa e o módulo para de ser lido. |

**Critérios de aceitação**

- *Read que falha dispara clock-out + `begin()`, no máx. 1/ciclo* — `loop`, seção 3:
  o `bool i2c_recovered` é o orçamento; o primeiro read que falha (IMU, depois baro)
  o gasta.
- *`i2c_bus_recover` copiado do ELE3km, sem GPIO literal fora de `pins.h`* — a grep 5
  de `DISCIPLINE.md` sai vazia; o adaptador usa os símbolos de `pins.h`.
- *Módulo cujo `begin()` falha fica ausente e para de ser lido* — a flag recebe o
  retorno de `begin()`; a leitura do próximo ciclo é guardada por `if (g_*_ok)`.
- *Sem contador de reinit, sem retentativa de 5 s* — verificável por revisão: nada no
  laço reergue a flag nem agenda retry; o backstop é o watchdog/reboot da issue 02.

**Decisões, tomadas dentro do escopo da issue**

*Ordem de recuperação = ordem de leitura do laço (IMU antes do baro).* A issue pede
"no máximo um módulo recuperado por ciclo" mas não nomeia o vencedor quando os dois
falham no mesmo ciclo. O laço já lê a IMU a cada volta e o baro num subciclo de
25 Hz, então o orçamento cai naturalmente para a IMU primeiro — o mesmo corte da
issue 09 do ELE3km, sem inventar uma prioridade nova.

*Uma linha de `Serial` por recuperação.* O AC de bancada (soltar/reconectar o baro
provoca uma recuperação e a altitude volta) precisa que o operador **veja** o evento
discreto: uma recuperação de ~10–50 ms pode disparar e resolver entre dois despejos
de diagnóstico de 250 ms, invisível no `print_sensor_sample`. A linha
`i2c: recover <peça> @ <ms> -> ok|AUSENTE` torna o evento observável, no mesmo estilo
de diagnóstico de bancada que o resto do `main.cpp` já usa. Não é contador durável
nem carimbo no cartão (esses eram diagnóstico da issue 10 do ELE3km, fora daqui).

**Fila de bancada** (o AC de target, fora do `/tdd`): soltar/reconectar o baro no
barramento provoca uma recuperação e a altitude volta sem reset da placa. Precisa de
placa + BMP280 + um jeito de segurar/soltar SDA (jumper à mão). ⚠️ Só ligar com a
antena do SX1276 conectada.

**Pendência de higiene (não feita, fora do escopo):** o cabeçalho de
`src/hal/i2c_bus.h` foi copiado do ELE3km e ainda cita "issue 09"/"issue 10" e a
política de retentativa de 5 s + máquina de saúde `{OK, DEGRADED, FAILED}` — que o
fallback **não** tem (AC4). O mecanismo está correto; só o texto do comentário
descreve o projeto errado. Vale uma correção de comentário à parte.
