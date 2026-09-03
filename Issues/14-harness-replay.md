# 14 — Harness de replay

**Tipo:** AFK
**User stories:** 102, 103

## What to build

A ferramenta que transforma o primeiro voo real no teste de regressão de todas as versões futuras do estimador.

O registro de 64 B carrega os valores **brutos** *e* os fundidos. Isso torna possível reproduzir um log de voo real através do `core/` no host, comparando as saídas recalculadas com as que o firmware produziu em voo. Nenhum dado sintético iguala esse caso de teste: ele contém o ruído real da IMU sob boost, os vãos reais de GPS, e a resposta real do barômetro na porta estática instalada.

O harness lê um arquivo de log bruto do cartão, usa a varredura de recuperação da issue 04 para extrair os registros válidos, reconstrói a sequência de `SensorSample` a partir dos campos brutos, e alimenta `FlightComputer::update()` no host com o timestamp de cada registro.

**Modos de uso:**

- **Comparação:** roda o núcleo atual sobre um log antigo e reporta as divergências em altitude, posição, fonte de posição e fase. É assim que uma mudança no estimador é avaliada.
- **Regressão:** um log de referência com as saídas esperadas vira caso de teste na suíte nativa, falhando se uma mudança alterar o comportamento além de uma tolerância.

Este é o item de maior valor de longo prazo do projeto, e é também a justificativa concreta do requisito escrito da issue 04:

> O registro de log deve ser suficiente para reconstruir a amostra de sensores que o produziu. Nenhum campo bruto pode ser removido sob o argumento de que já está na saída fundida.

Se alguém remover um campo bruto por parecer redundante, este harness para de funcionar e o primeiro voo deixa de ser reutilizável.

## Acceptance criteria

- [ ] Ferramenta no ambiente `native` que lê um arquivo de log bruto e extrai os registros válidos pela varredura de magic + CRC + contador de boot
- [ ] Reconstrução de `SensorSample` a partir dos campos brutos de cada registro
- [ ] Alimentação de `FlightComputer::update()` com o timestamp de aquisição de cada registro
- [ ] Modo de comparação: reporta divergências em altitude, posição, fonte de posição e fase entre a saída recalculada e a gravada
- [ ] Modo de regressão: um log de referência integrado à suíte nativa, falhando se a saída divergir além de uma tolerância declarada
- [ ] Teste: um log sintético gerado pelo próprio núcleo, reproduzido pelo harness, devolve saídas idênticas — o ciclo fecha
- [ ] Documentação de como adicionar um log de voo real como caso de regressão
- [ ] Verificação explícita, com falha se violada, de que todos os campos brutos necessários à reconstrução estão presentes no registro

## Blocked by

- Issue 04 (log — formato do registro e varredura de recuperação)
- Issue 07 (estimador — é o que o harness existe para avaliar)

## Estado da implementação

Fechada em 2026-08-20 por `/tdd`. Diferente das issues de hardware, esta é toda
host/native — não tem fila de bancada. 5 novos casos em `test_replay` (102 no total).

**Entregue:**

- **Reconstrução** (`core/replay.{h,cpp}`, puro): `sensor_sample_from_record()`
  remonta a `SensorSample` a partir dos campos brutos do registro de 64 B, com
  `imu_valid` recuperado do bit de saúde `kImu`.
- **Comparação** (AC 4): `replay_and_compare()` reproduz os registros por uma
  `FlightComputer` nova, alimentando o `t_ms` de aquisição de cada um, e agrega as
  divergências em altitude, posição fundida, fonte e fase contra uma
  `ReplayTolerance` declarada. Puro e sem alocação.
- **Ciclo fechado** (AC 6): `test_synthetic_flight_replays_identically` gera um voo
  pelo núcleo, serializa (header + registros), varre de volta com `scan_records`, e
  reproduz — divergência zero sob tolerância exata. Prova o pipeline inteiro.
- **Regressão de núcleo alterado** (AC 4/5): `test_comparison_reports_divergence…`
  reproduz um log com um limiar de liftoff alterado e confirma que a fase (enum) é
  exata — nenhuma tolerância a perdoa.
- **Guarda de suficiência de campos** (AC 8): `record_reconstruction_fields_present()`
  faz a volta encode→scan→reconstrói e falha se um campo bruto sair do formato de
  64 B. Checado no arranque do `tools/replay` e exigido pela suíte.
- **Ferramenta de host** (AC 1): `tools/replay ARQUIVO.BIN [tol_alt] [tol_pos]` lê o
  binário, separa o voo pelo contador de boot, reproduz e imprime o relatório; sai
  não-zero na divergência (usável em CI). Linka o `core/` puro direto, como o
  `recover_log`. Fumaçado ponta a ponta com um `.BIN` sintético.
- **Documentação** (AC 7): `tools/REPLAY.md` — como rodar e como adicionar um voo
  real como caso de regressão.

**Descoberta que precisou da sua decisão (regra de escopo do `NEXT.md`):** o registro
de 64 B **não carrega** dois campos brutos que a `FlightComputer` consome —
`gps.altitude_m` (corrige o canal vertical do estimador a cada fix) e
`accel_saturated` (do ADC bruto, perdido na conversão para mg). O requisito escrito
da issue 04 ("nenhum campo bruto pode ser removido") já nascia furado para esses
dois. **Decisão autorizada:** construir o harness contra os campos que o registro
carrega e tratar os dois como **lacunas conhecidas e documentadas**, com o guarda de
AC 8 protegendo os campos hoje presentes; a reprodução é exata para o subconjunto
reconstruível (o que fecha o ciclo sintético) e diverge nas janelas de GPS/boost. A
alternativa — mudar o formato congelado do cartão para carregar os dois — é uma
decisão de contrato à parte (mexe em `log_codec.h`, no `recover_log`, e obriga a
versionar o formato), fora do escopo desta issue.

**Nota de fidelidade:** o harness reproduz só os ciclos que estão no cartão. Em voo
todo ciclo é gravado (a reprodução do voo é exata); pousado o log cai para 1 Hz
(issue 13) e a sequência reproduzida é subamostrada — mas a regressão do estimador
vive no voo, não no repouso.
