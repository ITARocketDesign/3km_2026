# Ordem de execução — próxima issue

Este arquivo é o **cursor de execução** do projeto. Quando eu disser
**"execute next issue"** (com `/tdd`), o agente lê este arquivo, pega a issue
marcada com **▶**, e a implementa. Nada mais precisa ser dito.

---

## ⏭️ Começe por aqui (2026-08-20)

**A fila `/tdd` está vazia — não há mais `▶`.** As 14 issues de código (01–14) estão
implementadas, com 102 casos nativos passando, o target compilando e os cinco greps
de `DISCIPLINE.md` vazios. Não existe "próxima issue" para `/tdd`; o próximo trabalho
é uma escolha entre três frentes, nenhuma delas de `/tdd`:

1. **Bancada** — verificação no target das issues 01–13 (mais o comportamento de
   hardware pós-pouso da 13). Precisa da placa, das duas antenas, receptores, cartão,
   IMU e GPS. Tudo listado na seção **Fila de bancada** no fim deste arquivo. É o
   maior bloco de valor não-realizado: há firmware que voa, falta prová-lo no metal.
2. **HITL (15, 16)** — não é firmware, não passa por `/tdd`. São documentos para
   outras equipes; me avise e eu produzo o documento. A 15 (porta estática) é
   independente e tem prazo de fabricação próprio.
3. **Decisão de contrato adiada pela 14** — levar `gps.altitude_m` e
   `accel_saturated` ao formato congelado do cartão (`log_codec.h`), para o harness
   de replay reproduzir voos reais bit a bit. É trabalho de código, mas mexe num
   contrato congelado (formato do cartão, `recover_log`, versionamento) — exige
   decisão antes, não é `/tdd` direto. Ver o *Estado da implementação* da issue 14.

Me diga qual frente amanhã. Se for bancada ou HITL, não é `/tdd`; se for a decisão de
contrato, começamos pela decisão.

---

## Regra de escopo — vale para toda issue, sem exceção

**Implemente exatamente o que os critérios de aceitação da issue pedem. Nada além.**

Se durante a implementação surgir **qualquer decisão que não esteja resolvida
no texto da issue** — escolha de arquitetura, mudança no `core/`, alteração de
formato no ar (`PACKET_FORMAT.md`) ou no cartão (`log_codec.h`), novo pino,
nova dependência, comportamento de hardware não especificado, ou uma pendência
que a issue explicitamente deixou de fora — **pare e me pergunte antes de
prosseguir.** Não escolha um default "razoável" por conta própria. Não amplie,
não estreite e não transforme o escopo da issue.

Motivo: cinco das oito regras de `ELE3km/DISCIPLINE.md` custam hardware se
violadas, e três dos contratos do projeto (pacote, log, invariantes do `core/`)
são fronteiras com outras equipes ou ferramentas. Uma decisão silenciosa aqui
não é economia de tempo — é risco de PA queimado ou de contrato quebrado.

## Leitura obrigatória antes de escrever código (toda issue)

- O próprio arquivo da issue, do começo ao fim, incluindo a seção final
  **Estado da implementação** se já existir.
- `Docs/ELE3km_firmware_PRD_v3.md` (as user stories citadas no topo da issue)
- `Docs/ELE3km_connections.md` (netlist — fonte única do mapa de pinos)
- `Docs/ELE3km_hardware_constraints.md` (hazards H1–H16, arbitragem C1–C6)
- `ELE3km/DISCIPLINE.md` (as oito regras) e, quando a issue toca o ar ou o
  cartão, `ELE3km/PACKET_FORMAT.md` e o cabeçalho de `ELE3km/src/core/log_codec.h`.

## Definição de pronto (toda issue)

1. Suíte nativa passa: `cd ELE3km && ~/.platformio/penv/bin/pio test -e native`
2. Target compila: `cd ELE3km && ~/.platformio/penv/bin/pio run -e heltec_wifi_lora_32_V2`
3. Os cinco greps do fim de `ELE3km/DISCIPLINE.md` saem vazios.
4. A issue ganha (ou atualiza) sua seção **Estado da implementação**.
5. Este arquivo é atualizado: a issue vira ✅ com a data, e o **▶** desce para
   a próxima.

O critério de **verificação no target** (bancada com placa, antenas, receptores)
**não** é pré-requisito para fechar a implementação por `/tdd` — ele fica na fila
de bancada abaixo. Nenhuma issue de código espera hardware para ser implementada.

---

## Fila de execução (`/tdd`)

Ordem: caminho crítico primeiro (`11 → 12`), depois o ramo de falhas
(`09 → 10`), depois os dois itens soltos. As dependências abaixo já estão todas
satisfeitas na posição em que a issue aparece.

| Ordem | Issue | Título | Dep. | Status |
|:---:|:---:|---|:---:|---|
| ✅ | [11](11-watchdog-stack-brownout.md) | Watchdog, stack e brown-out | 05 ✅ | ✅ 2026-08-17 (Opção 1) |
| ✅ | [12](12-boot-loop-beacon-sobrevivencia.md) | Boot loop + beacon de sobrevivência | 03 ✅, 11 ✅ | ✅ 2026-08-19 |
| ✅ | [09](09-recuperacao-barramento-i2c.md) | Recuperação do barramento I²C | 07 ✅ | ✅ 2026-08-19 |
| ✅ | [10](10-saude-e-degradacao.md) | Saúde e degradação por subsistema | 04 ✅, 09 ✅ | ✅ 2026-08-19 (fatia de núcleo) |
| ✅ | [13](13-comportamento-pos-pouso.md) | Pós-pouso: Stationary, média, beacon | 03 ✅, 06 ✅, 08 ✅ | ✅ 2026-08-20 (fatia de núcleo) |
| ✅ | [14](14-harness-replay.md) | Harness de replay | 04 ✅, 07 ✅ | ✅ 2026-08-20 |

**A fila `/tdd` está vazia.** As 14 issues de código (01–14) estão implementadas e
com suíte nativa passando. O que resta não passa por `/tdd`: a **fila de bancada**
(verificação no target de 01–13, mais o comportamento de hardware pós-pouso da 13) e
as **duas HITL** (15, 16), ambas descritas abaixo. Se nada disso, o próximo trabalho
de código é a decisão de contrato adiada: levar `gps.altitude_m` e `accel_saturated`
ao formato do cartão para o harness da 14 reproduzir voos reais bit a bit.

Notas de ordem:

- **11** fechada em 2026-08-17 na **Opção 1** (subconjunto viável no framework
  fixado — arduino-esp32 2.0.17 / IDF 4.4). Vários ACs foram reescritos por serem
  infactíveis sem trocar de framework; ver o *Estado da implementação* da issue.
  Dois itens ficam para uma decisão de re-plataforma/bancada: brown-out em `SEL_0`
  em vez de 2,43 V, e timeout único de 5 s em vez de 3 s/5 s por task.
- **12** fechada em 2026-08-19. A janela usa relógio RTC + `RTC_NOINIT_ATTR`, sem
  escrita de timestamp na NVS; o evento fica nos bytes antes reservados do
  cabeçalho do log. Os dois testes físicos continuam na fila de bancada.
- **11 e 12** fecharam o caminho crítico de implementação e entregam o marco
  "sobrevive a rail marginal sem ficar mudo"; falta validá-lo no target.
- **09** fechada em 2026-08-19. Entrega só o mecanismo (`hal/i2c_bus`) e a guarda
  de um módulo por ciclo; metade das ACs já vinha de fatias anteriores. O carimbo
  durável do erro de I²C no cartão e a cadência de retentativa de 5 s são da **10**,
  que chama `i2c_bus_recover()` como primitiva. Três testes de bancada na fila.
- **10** fechada em 2026-08-19 na **fatia de núcleo**. Entrega a máquina de saúde
  `{OK, DEGRADED, FAILED}` (`core/health.h`, retry de 5 s, reverificação de config,
  contador de reinit) e o bitmap de 6 bits completo no pacote **e** no registro,
  tudo testado no `native` (90 casos). Duas decisões autorizadas: o contador de
  reinit fica em runtime + bitmap (o registro de 64 B está cheio e congelado), e o
  refactor de interface dos drivers (`verify_config()`, adoção da máquina, troca da
  cadência de recuperação I²C de imediata para 5 s) vai para a fila de bancada. Ver
  o *Estado da implementação* da issue.
- **13** fechada em 2026-08-20 na **fatia de núcleo**. Entrega o filtro e a média
  de fixes pós-pouso (extensão da máquina de fonte de posição: sat ≥ 4 e HDOP ≤ 5,0,
  média dos aceitos, campo de amostras 0–7), o overload dos 3 bits de qualidade de
  fix do byte 17 para carregar as amostras em POUSADO, e o `should_log` que derruba
  a taxa de log para 1 Hz pousado — tudo no `native`. Três decisões autorizadas
  antes de codar: overload de campo em vez de campo novo, seam de log no núcleo, e
  **adiar a divergência de cadência dos rádios** (E22 20 s / SX1276 5 s) para a
  bancada, junto do GPS→Stationary e do SF físico. Ver o *Estado da implementação*
  da issue.
- **14** fechada em 2026-08-20 (host/native, sem fila de bancada). Entrega a
  reconstrução de amostra, a comparação com tolerância declarada, o ciclo fechado
  sintético, o guarda de suficiência de campos e a ferramenta `tools/replay` +
  `tools/REPLAY.md`. **Descoberta que precisou de decisão:** o registro de 64 B não
  carrega `gps.altitude_m` nem `accel_saturated`, então um voo real não reproduz bit
  a bit. Decisão autorizada: tratar as duas como lacunas conhecidas e documentadas
  (o harness reproduz o subconjunto reconstruível, exato; o guarda protege os campos
  presentes), e deixar a mudança do formato congelado como decisão de contrato à
  parte. Ver o *Estado da implementação* da issue.
- Se você quiser outra ordem, só reordenar a tabela e mover o **▶**.

---

## Fora da fila `/tdd`

**HITL — não é firmware, não passa por `/tdd`.** São entregas para outras
equipes; me avise quando quiser trabalhá-las e eu produzo o documento, não código.

- [15](15-hitl-porta-estatica.md) — especificação da porta estática (equipe de
  estrutura). Independente de tudo e com prazo de fabricação próprio: vale
  destravar cedo.
- [16](16-hitl-contrato-estacao-solo.md) — contrato imposto à estação de solo
  (equipe de solo). Insumo pronto em `ELE3km/PACKET_FORMAT.md`.

**Fila de bancada — verificação no target das issues 01–13.** Todas
implementadas e com suíte nativa passando; falta só confirmar no hardware, numa
sessão única de bancada (placa + um receptor de 915 MHz para o SX1276 e um de
433 MHz para o E22 + **as duas antenas** +
cartão microSD + MPU6050 + um jumper para segurar SDA). A 09 acrescenta os testes
de barramento travado (SDA presa destrava, IMU removida e o baro segue no ar,
recuperação não estoura o ciclo); a 11, watchdog/watermark; a 12, três resets em
30 s e power-on de saída do modo. A **10** acrescenta a adoção da máquina de saúde
por driver: `verify_config()` relendo os registradores de configuração de BMP280 e
MPU6050 (sensor que volta de fábrica é pego pela config, não pelos dados), a troca
da cadência de recuperação I²C da imediata (09) para a de 5 s da máquina, e o
carimbo dos contadores de reinit no Serial. A **13** acrescenta o comportamento
pós-pouso de hardware: GPS reconfigurado para `dynModel = Stationary` ao pousar, a
cadência divergente dos rádios (E22 em SF12 a 1 pacote/20 s como beacon primário,
SX1276 a ~1 pacote/5 s, defasagem de 500 ms mantida — exige um modo pós-pouso com
períodos por rádio no `tx_scheduler`), a adoção do `should_log` pela task de I/O, e
o teste de que o beacon roda por horas sem travar. ⚠️ A partir da 03 a placa aciona
o PA de 1 W — **nunca ligar sem as duas antenas conectadas.** Isto não é trabalho de
`/tdd`.
