# 12 — Detector de boot loop e beacon de sobrevivência

**Tipo:** AFK
**User stories:** 10, 86

## What to build

A única exceção à política fail-active de dois rádios, e a razão de ela existir.

**O cenário.** Se o rail de 3,3 V está marginal — bateria baixa, resistência parasita alta (hazard H15) — o PA de 1 W do E22 causa brown-out a cada transmissão. O firmware reseta, reinicializa, tenta transmitir, e reseta de novo. A equipe de recuperação fica em **silêncio total**, exatamente quando mais precisa do beacon.

**Detecção.** No boot, **antes de inicializar o E22**, ler o boot counter da NVS e o histórico de resets retido no domínio RTC. Se houve **3 ou mais resets em menos de 30 s**, declarar boot loop.

**Ação:**

- **Não inicializar o E22.** Ele é o causador provável do sag no rail U6; sem ele, o rail estabiliza.
- Transmitir **somente pelo SX1276**, que vive no rail de 5 V do U7, independente do U6.
- Cadência: **SF12, 1 pacote a cada 20 s** — beacon de sobrevivência, máxima sensibilidade.

**Por que o E22 é o sacrificado e não o SX1276.** O E22 está no rail que afunda. O SX1276 está num buck diferente, alimentado pelo mesmo nó de bateria mas independente do U6. Desabilitar o E22 estabiliza o rail e o sistema volta a operar; o SX1276 sobrevive porque não compartilha o rail em sag. Isso transforma silêncio total num beacon degradado mas funcional.

**Esta é a única quebra da política fail-active de §5** — e é por falha sistêmica do rail, não por detecção de falha do rádio. Não abre precedente para failover por detecção.

**Saída do modo:** apenas um power-on limpo (desligar e religar a chave). Isso garante que a equipe verificou a bateria ou o hardware antes de reabilitar o E22.

**Logging:** o evento de boot loop é registrado no **cabeçalho do SD** deste boot, com o boot counter e a contagem de resets recentes. Os bytes 9 e 10, antes reservados, carregam o evento sem alterar o registro congelado de 64 B.

**Endurance de NVS:** o único write em NVS continua sendo o `BootCounter`
existente, com um commit por boot. O histórico temporal usa RTC slow memory
(`RTC_NOINIT_ATTR`) e não acrescenta desgaste de flash.

## Decisão de dependência — resolvida em 2026-08-19

A issue 11 foi fechada na **Opção 1**: o handler pré-reset do watchdog e o hook de
stack overflow que **persistiriam o timestamp do último reset em NVS foram
removidos** (rodariam em contexto de ISR, onde escrita em NVS não é segura no
framework fixado — arduino-esp32 2.0.17 / IDF 4.4). Ver o *Estado da implementação*
da issue 11.

Consequência direta: o boot counter continua em NVS (`hal/boot_counter.h`), mas a
janela de timestamps passa a usar `esp_rtc_get_time_us()` e `RTC_NOINIT_ATTR`.
`RTC_DATA_ATTR` foi descartado porque o framework instalado só garante sua retenção
em deep sleep; `RTC_NOINIT_ATTR` é a seção explicitamente não reinicializada após
restart. Um power-on limpo apaga a janela explicitamente. A janela não escreve flash.

A **lógica de detecção** (AC de teste nativo: 3 resets em 30 s a partir de sequências
sintéticas de timestamps) é pura e **independente da fonte** — ela recebe timestamps
e decide. Foi exercitada por `/tdd`, e a origem dos timestamps está resolvida pela
decisão acima, sem dependência adicional de hardware para fechar a implementação.

## Acceptance criteria

- [x] Leitura do boot counter da NVS e do histórico RTC **antes** de qualquer inicialização do E22
- [x] 3 ou mais resets em menos de 30 s declara boot loop
- [x] Em boot loop, o E22 **não é inicializado** em nenhum caminho de código
- [x] Em boot loop, o SX1276 transmite sozinho em SF12, 1 pacote a cada 20 s
- [x] O evento de boot loop é gravado no cabeçalho do SD, com boot counter e contagem de resets
- [x] Saída do modo apenas por power-on limpo — nenhuma saída automática
- [x] Fora de boot loop, a política fail-active de §5 permanece intacta: nenhuma outra lógica de detecção acopla os dois rádios
- [x] Teste nativo: a lógica de detecção (3 resets em 30 s) exercitada com sequências sintéticas de timestamps
- [ ] Teste no target: três resets forçados em menos de 30 s levam ao modo boot loop, com o E22 silencioso e o SX1276 transmitindo a cada 20 s
- [ ] Teste no target: power-on limpo depois do boot loop reabilita o E22

## Blocked by

- Issue 03 (E22 — é o rádio que o detector desabilita)
- Issue 11 (watchdog e stack hook). **Atenção:** fechada na Opção 1, ela NÃO
  produz o timestamp de reset em NVS que este detector supunha consumir — ver a
  Decisão de dependência acima. Fornece só o boot counter.

## Estado da implementação

**Implementada em 2026-08-19 via `/tdd`.** A suíte nativa passa com 81 casos em
10 suítes, o target compila (RAM 26,1 %, flash 29,0 %) e os cinco greps de
`DISCIPLINE.md` saem vazios.

### O que entrou

- `core/boot_loop.{h,cpp}`: detector puro; recebe timestamps sintéticos e devolve
  `{active, recent_reset_count}`. Três resets em janela aberta de 30 s disparam;
  exatamente 30 s não dispara.
- `hal/boot_loop.{h,cpp}`: janela de três carimbos em `RTC_NOINIT_ATTR`, relógio
  `esp_rtc_get_time_us()`, magic de validação e latch que só power-on limpa.
- `core/tx_scheduler.{h,cpp}`: modo explícito `SurvivalBeacon`; nenhum slot do E22,
  apenas SX1276 a cada 20 s, cobrando o airtime conservador de 1712 ms de SF12.
- `hal/radio_sx1276.{h,cpp}`: seleção Flight/SF7 ou SurvivalBeacon/SF12; recovery
  preserva o modo escolhido.
- `main.cpp`: decisão e boot counter antes do E22; no modo de sobrevivência
  `RadioE22::begin()` não é chamado e o E22 permanece em reset desde
  `board_early_init()`.
- Cabeçalho do log: `boot_loop` no byte 9 e `recent_reset_count` no byte 10,
  ambos cobertos pelo CRC. Leitores v1 antigos ignoram os bytes antes reservados;
  o registro de 64 B e o formato no ar não mudaram.

### Verificação no target pendente

- [ ] Três resets forçados em menos de 30 s: E22 permanece em reset e o SX1276
      transmite em SF12 a cada 20 s.
- [ ] Power-on limpo depois do modo de sobrevivência: histórico apagado, E22
      reabilitado e política fail-active normal restaurada.

⚠️ A primeira sequência de resets começa fora do modo de sobrevivência e portanto
inicializa o PA de 1 W. Fazer a bancada somente com **as duas antenas conectadas**.
