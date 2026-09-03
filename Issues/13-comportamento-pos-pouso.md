# 13 — Comportamento pós-pouso: Stationary, média filtrada e beacon divergente

**Tipo:** AFK
**User stories:** 1, 5, 6, 7, 8, 9, 31, 44, 71

## What to build

O modo de recuperação. Depois de POUSADO confirmado, quase todas as configurações de voo se invertem — o objetivo deixa de ser acompanhar a trajetória e passa a ser levar uma pessoa até o foguete na mata.

**GPS para Stationary.** Ao entrar em POUSADO, `UBX-CFG-NAV5` com `dynModel = Stationary`. O modelo Airborne diz ao receptor "espere aceleração" e **impede** a filtragem pesada que dá boa precisão parado.

**Posição de recuperação por média filtrada de fixes.** Sob copa de árvore o GPS dá fixes de qualidade muito variável, e o último costuma ser o pior — transmitir o último fix é uma aposta.

- **Critério de aceitação na média:** satélites **≥ 4** E HDOP **≤ 5,0**. Quatro satélites é o mínimo para solução 3D; HDOP 5,0 corresponde a ~10–15 m de erro horizontal, e abaixo disso o fix é ruim demais para melhorar a média.
- **Transição contínua, sem flag binário "pronta/não pronta":**

| Fixes filtrados acumulados | O que é transmitido |
|---|---|
| **0** | Última posição GPS válida **de voo** + altitude barométrica da descida |
| **1–2** | Média parcial, com o campo de amostras indicando 1 ou 2 |
| **≥ 3** | Média considerada confiável |

- O **campo de amostras** vai nos 3 bits de qualidade de fix do byte 17. Com HDOP e idade, é o que diz ao operador se ele caminha para um ponto ou para um círculo de 100 m. **A decisão de caminhar fica com ele, não com o firmware.**
- Com zero fixes pós-pouso, a última posição de voo ainda é útil: um fix a 200 m na descida limita o ponto de pouso a um raio pequeno.
- A reconfiguração para Stationary melhora a qualidade dos fixes parado, mas **não substitui a filtragem** — sob copa densa o modelo Stationary ainda entrega fixes ruins.

**Beacon divergente.** Os dois rádios deixam de ser espelhos:

| | E22 / 433 MHz | SX1276 / 915 MHz |
|---|---|---|
| Papel | Beacon primário — 433 penetra folhagem | Redundância de banda |
| Configuração | SF12, máxima sensibilidade | cadência intermediária |
| Cadência | 1 pacote a cada 20 s | ~1 pacote a cada 5 s |

A defasagem entre eles se mantém, para não somar consumo no nó de bateria.

**Taxa de log cai para 1 Hz.** Sem isso o beacon grava horas de dados parados a 6,4 kB/s.

A autonomia do beacon é o que importa aqui: a busca pode durar mais que o voo.

## Acceptance criteria

- [ ] Ao entrar em POUSADO, GPS reconfigurado para `dynModel = Stationary`
- [ ] Filtro de fixes pós-pouso: aceita se e somente se satélites ≥ 4 **e** HDOP ≤ 5,0
- [ ] Média acumulada dos fixes aceitos é a posição transmitida
- [ ] 0 fixes → última posição GPS válida de voo + altitude barométrica da descida
- [ ] 1–2 fixes → média parcial, com o campo de amostras correto
- [ ] ≥ 3 fixes → média considerada confiável
- [ ] Campo de amostras codificado nos 3 bits de qualidade de fix do byte 17
- [ ] E22 muda para SF12, cadência 1 pacote a cada 20 s
- [ ] SX1276 muda para cadência intermediária ~1 pacote a cada 5 s
- [ ] Defasagem entre os dois rádios mantida
- [ ] Taxa de log cai para 1 Hz em POUSADO
- [ ] Teste nativo: fixes com sat < 4 ou HDOP > 5 são rejeitados
- [ ] Teste nativo: **a posição transmitida nunca inclui um fix rejeitado pela filtragem**
- [ ] Teste nativo: com 0 fixes bons, o pacote carrega a última posição de voo
- [ ] Teste nativo: com 1–2 fixes, a média parcial é transmitida com o campo de amostras correto
- [ ] Teste nativo: com ≥ 3 fixes, a média é estável
- [ ] Teste nativo: a cadência de log cai para 1 Hz ao entrar em POUSADO
- [ ] Teste no target: beacon roda por horas sem travar nem parar

## Blocked by

- Issue 03 (rádios — a divergência de cadência é configuração dos dois)
- Issue 06 (fases — POUSADO é o gatilho)
- Issue 08 (fonte de posição — a média pós-pouso é uma extensão dessa máquina)

## Estado da implementação

Fechada em 2026-08-20 na **fatia de núcleo** (`/tdd`). Entrega a lógica pura pós-pouso
com testes nativos; a configuração de hardware e a divergência de cadência dos rádios
ficam na fila de bancada.

**Entregue no núcleo (`native`, 97 casos no total, 6 novos aqui):**

- **Filtro e média pós-pouso**, como extensão de `PositionSourceMachine`
  (`core/position_source.{h,cpp}`). Em POUSADO a máquina troca de regime: aceita um
  fix se e somente se **satélites ≥ 4 e HDOP ≤ 5,0** (`hdop_half ≤ 10`), acumula os
  aceitos numa **média** (somas em `int64` — um voo de busca acumula milhares de
  fixes) e transmite a média. Um fix rejeitado nunca entra na posição transmitida
  nem sobe a contagem. Os fixes pós-pouso **não** tocam o último fix de voo: com 0
  amostras aceitas a saída é `LastValid` com a última posição de GPS **válida de
  voo**. Testado: rejeição por sat/HDOP, exclusão do rejeitado da média, contagem
  1→2→3, saturação em 7, e o fallback de 0 amostras.
- **Campo de amostras nos 3 bits de qualidade de fix do byte 17.** Em POUSADO o
  `FlightComputer` escreve o número de amostras (0–7, saturado) no lugar da
  qualidade GGA; a fase (também no byte de flags) desambigua. **Overload de campo,
  não campo novo** — os 20 B são orçamento de airtime congelado. `PACKET_FORMAT.md`
  e o cabeçalho de `telemetry_codec.h` foram atualizados; a estação de solo
  (issue 16) precisa ler os bits 5–7 pela fase.
- **Log a 1 Hz em POUSADO.** `UpdateResult` ganhou `should_log`: em voo é `true`
  todo ciclo (nenhuma amostra descartada), em POUSADO cai para 1 registro por
  segundo (`FlightComputerConfig::landed_log_period_ms = 1000`), mesmo a 25 Hz de
  aquisição. A **telemetria não é afetada** — ela tem a própria cadência no
  escalonador; `should_log` gate só a gravação no cartão.

**Três decisões de mecanismo, confirmadas antes de codar (regra de escopo do
`NEXT.md`):** (1) o overload do byte 17 por fase em vez de campo novo; (2) o seam
`should_log` no núcleo em vez de deixar a taxa de log para a task de I/O; (3)
**adiar** a divergência de cadência dos rádios para a bancada.

**Fila de bancada (target, não é `/tdd`):**

- **GPS → Stationary** ao entrar em POUSADO (`UBX-CFG-NAV5`, `dynModel = Stationary`)
  no driver `hal/gps_neo6m`.
- **Cadência divergente dos rádios:** E22 em **SF12, 1 pacote/20 s** (beacon
  primário) e SX1276 em **~1 pacote/5 s** (redundância), com a defasagem de 500 ms
  mantida. Hoje o `tx_scheduler` tem um período único compartilhado e o modo
  `SurvivalBeacon` (só SX1276). Um modo pós-pouso com **períodos por rádio** é um
  redesenho do escalonador; nenhum AC de teste nativo o cobre (só a config SF12 e o
  teste de bancada "beacon roda por horas"), então ficou para a bancada junto com o
  SF físico. A adoção do `should_log` pela task de I/O também entra aqui.
- **Teste de bancada:** beacon roda por horas sem travar nem parar.

**ACs de teste nativo cobertos:** fix com sat < 4 ou HDOP > 5 rejeitado; a posição
transmitida nunca inclui um fix rejeitado; 0 fixes → última posição de voo; 1–2
fixes → média parcial com amostras corretas; ≥ 3 → média estável; cadência de log
cai para 1 Hz ao entrar em POUSADO.
