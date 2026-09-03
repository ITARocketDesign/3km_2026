# 09 — Beacon de sobrevivência por boot-loop + watchdog de cadência de TX

**Tipo:** Bancada
**User stories:** 9, 10 (num rádio só, SF12)

## What to build

As duas redes de segurança contra silêncio total: um caminho de boot mínimo quando a
placa entra em loop de reset, e um watchdog que transforma um travamento de TX em
reboot rápido.

- **Contador de boot em memória RTC** (sobrevive ao reset sem desgastar o flash):
  conta resets numa janela curta. Acima de um limiar, o boot entra num **caminho
  só-rádio**: inicializa **apenas o SX1276**, pula SD, GPS e bring-up de sensores, e
  emite um **beacon lento em SF12** — última posição conhecida da RTC, se houver;
  senão, só-altitude com a última pressão conhecida ou zero. É o modo que consegue
  botar um sinal no ar quando um rail marginal não deixa o boot completo terminar.
  - Requer o operador trocar o receptor para SF12 (já é um recompile de uma linha no
    `3km913hzReceiver`). Documentar isso no `README`.
- **Watchdog de cadência de TX:** se nenhum pacote chegou ao rádio em N s (N a
  definir, ordem de 5–10 s), força um reset. Um travamento duro do cartão segura a
  VSPI e nenhum firmware transmite durante ele (rev. 1, barramento compartilhado);
  este watchdog converte o silêncio num reboot-para-beacon rápido, em vez de esperar
  só o TWDT genérico.

Sem detecção de brownout sofisticada e sem a lógica de fase do beacon do ELE3km — só
o contador de boot da RTC e o limiar.

## Acceptance criteria

- [ ] Contador de boot em memória RTC, incrementado no boot, zerado após um voo
      estável (ex.: após N segundos sem reset)
- [ ] Acima do limiar → caminho só-rádio: inicializa só o SX1276, pula SD/GPS/sensores
- [ ] Beacon SF12 lento com última posição da RTC se houver, senão só-altitude
- [ ] Watchdog de cadência de TX: sem pacote ao rádio em N s → reset
- [ ] `README` documenta que o beacon é SF12 e o receptor precisa trocar para SF12
- [ ] Teste nativo: a lógica pura de "contou N resets na janela → modo beacon" e a de
      "sem TX em N s → pede reset" são testáveis sem hardware (tempo por parâmetro)
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: forçar resets repetidos entra no beacon SF12; um receptor em SF12 o
      ouve
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 06 (SD/última posição), 08 (saúde) — e o beacon reusa o TX da 03
