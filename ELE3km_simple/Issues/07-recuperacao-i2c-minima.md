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
