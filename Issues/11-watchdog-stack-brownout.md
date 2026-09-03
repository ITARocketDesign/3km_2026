# 11 — Watchdog, monitoramento de stack e brown-out

**Tipo:** AFK
**User stories:** 32, 83, 84, 87, 88

## What to build

As três defesas que transformam um travamento, um overflow ou uma queda de tensão num reboot diagnosticável, em vez de um foguete mudo ou de corrupção silenciosa.

### Watchdog (§17)

**Mecanismo:** Task Watchdog Timer do ESP-IDF (`esp_task_wdt`), que monitora tasks individuais e identifica **qual** travou.

**Timeouts diferenciados:**

- Task `flight`: **3 s** — não tem operações longas; generoso o bastante para cobrir recuperação de I²C mais retry, curto o bastante para detectar deadlock.
- Task `io`: **5 s** — precisa cobrir a pior stall de SD (500 ms) mais airtime de SF12 (1712 ms) mais margem.

**Feed:** `esp_task_wdt_reset()` uma vez por iteração do loop principal de cada task.

**Ação pré-reset, no handler, antes do restart:**

1. Incrementar boot counter em NVS
2. Persistir fase de voo atual e referência barométrica, se ainda não persistidas nesta fase
3. Persistir timestamp do reset (consumido pelo detector de boot loop da issue 12)
4. **NÃO tentar flush do SD, NÃO tocar no SPI** — se a task de I/O é a que travou, qualquer operação SPI piora o cenário

**Estado do SPI após reset:** absorvido pela sequência de boot existente (CS HIGH antes de `SPI.begin()`). O cartão pode ficar num estado interno inconsistente, mas o firmware cria arquivo novo por boot e a varredura por magic + CRC recupera o arquivo anterior.

### Stack (§14)

**O problema é silencioso:** um overflow na task de I/O não trava a task, ele sobrescreve dados adjacentes. O watchdog não dispara. SdFat e RadioLib consomem 2–4 KB em operações de mount/pre-allocate, o que torna isso plausível.

- **`configCHECK_FOR_STACK_OVERFLOW` método 2** habilitado em `FreeRTOSConfig.h`: o FreeRTOS preenche o stack com um padrão e verifica os últimos bytes a cada context switch. O hook `vApplicationStackOverflowHook` incrementa um contador em NVS e força `esp_restart()`.
- **`uxTaskGetStackHighWaterMark()` a cada ~1 s** em cada task. O resultado é logado no registro do SD como campo de diagnóstico — **não** no pacote de rádio, onde não cabe nos 20 B. Watermark abaixo de **512 bytes** marca a task como `DEGRADED` no bitmap de saúde interno.

### Brown-out (§21)

- Brown-out detector configurado explicitamente no limiar **mais baixo: 2,43 V** (`CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_3`). Isso maximiza a margem antes de um reset.
- **Não desabilitar o BOD.** A NVS depende do flash operando em tensão válida, e corrupção silenciosa de NVS em tensão marginal é pior que um reset limpo — a NVS é justamente o mecanismo que garante continuidade após reset. Um reset por BOD entra no mesmo caminho de recuperação que um reset por watchdog.

## Acceptance criteria

- [ ] TWDT do ESP-IDF armado nas duas tasks
- [ ] Timeout de 3 s na task `flight`, 5 s na task `io`
- [ ] `esp_task_wdt_reset()` chamado uma vez por iteração de cada loop principal
- [ ] Handler pré-reset incrementa boot counter, persiste fase e referência, persiste timestamp do reset
- [ ] Handler pré-reset **não** toca no SPI nem tenta flush do SD
- [ ] `configCHECK_FOR_STACK_OVERFLOW` método 2 habilitado
- [ ] `vApplicationStackOverflowHook` incrementa contador em NVS e chama `esp_restart()`
- [ ] `uxTaskGetStackHighWaterMark()` amostrado a ~1 s em cada task
- [ ] Watermark gravado no registro do SD, **ausente** do pacote de rádio
- [ ] Watermark abaixo de 512 B marca a task como `DEGRADED`
- [ ] Brown-out detector em 2,43 V no sdkconfig; BOD **não** desabilitado
- [ ] Contador de resets do estimador (issue 07) também presente no registro
- [ ] Teste no target: travamento induzido na task `flight` produz reset em ~3 s, com fase e referência preservadas em NVS
- [ ] Teste no target: travamento induzido na task `io` produz reset em ~5 s, e o arquivo de log anterior é recuperável pela varredura
- [ ] Teste no target: os watermarks logados mostram folga confortável nos stacks de 8 KB e 12 KB

## Blocked by

- Issue 05 (tasks FreeRTOS — não há o que vigiar antes das duas tasks existirem)

## Estado da implementação

**Implementada em 2026-08-17 pela via `/tdd`, na Opção 1** (subconjunto viável no
framework fixado, com os ACs infactíveis reescritos abaixo). O framework do target
é o **arduino-esp32 2.0.17 (ESP-IDF 4.4.7), pré-compilado**: o kernel e o
`sdkconfig` são estáticos, então vários ACs desta issue — escritos supondo edição
livre de `FreeRTOSConfig.h` e do `sdkconfig` — não são alcançáveis sem trocar de
framework (Opção 2, recusada por reabrir a validação de hardware das issues 01–08).

### O que entrou

- **Núcleo (nativo, TDD):** `core/stack_health.h` — a fronteira única do limiar de
  512 B (`kStackWatermarkMinBytes` + `stack_watermark_degraded()`). Suíte
  `test/test_stack_health/`. É o único pedaço puro da issue; o resto é cola de
  firmware verificável só no target.
- **TWDT (`src/main.cpp`):** as tasks `flight` e `io` se inscrevem
  (`esp_task_wdt_add`) e alimentam (`esp_task_wdt_reset`) uma vez por iteração.
- **Watermark de stack:** `uxTaskGetStackHighWaterMark()` amostrado a cada ~1 s em
  cada task; o valor bruto (em bytes) vai ao registro do SD nos offsets 58/60,
  **ausente** do pacote de rádio. A `io` publica o seu por um `std::atomic<uint16_t>`
  SPSC (io → flight); a `flight` carimba os dois no registro antes de codificar.
- **Diagnóstico DEGRADED:** só-de-log, conforme decidido — watermark abaixo de
  512 B emite um aviso único no Serial por task, e o valor bruto no registro é o
  que atravessa para o pós-voo. **Não** toca no bitmap de saúde da issue 10 (que
  não tem conceito de task) nem no pacote de rádio.
- **Contador de resets do estimador (issue 07):** já presente no registro
  (`estimator_resets`, offset 55) — inalterado, verificado.
- **Brown-out:** o BOD **não** foi desabilitado (segue ligado no default do
  framework). Ver a ressalva de nível abaixo.

### ACs reescritos (Opção 1) — a fila de bancada valida o que sobrou

- ~~Timeout de 3 s na `flight`, 5 s na `io`~~ → **timeout único global de 5 s.** A
  API TWDT do IDF 4.4 não tem timeout por task; 3 s global falso-dispararia a `io`
  (SD 500 ms + airtime SF12 1712 ms + margem). Timeout por task exige IDF 5.
  **Consequência:** deadlock na `flight` é pego em ~5 s, não ~3 s.
- ~~Handler pré-reset persiste fase/referência/timestamp~~ → **removido.** O único
  gancho pré-panic (`esp_task_wdt_isr_user_handler`) roda em contexto de ISR, onde
  escrita em NVS não é segura. Fase e referência já são persistidas continuamente
  pela issue 06 (liftoff e cada transição); o **timestamp do reset** que a issue 12
  queria deste handler **não** é gravado — ver nota para a issue 12 abaixo.
- ~~Boot counter incrementado no handler~~ → **incrementado no boot** (`setup()`,
  `BootCounter::next()`), como já era. Cada reset vira boot novo, arquivo novo, e a
  varredura por magic+CRC recupera o anterior — o intento da issue, sem handler.
- ~~`configCHECK_FOR_STACK_OVERFLOW` método 2 em `FreeRTOSConfig.h`~~ → **já ligado**
  no kernel pré-compilado (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`). Nada a
  editar; um `FreeRTOSConfig.h` de projeto é ignorado.
- ~~`vApplicationStackOverflowHook` custom (NVS + `esp_restart`)~~ → **não
  sobreposto.** O símbolo já é definido na lib pré-compilada (aborta → reset);
  redefini-lo no app conflita no link. Overflow ainda reinicia limpo; falta só o
  contador persistente.
- ~~Brown-out em 2,43 V (`CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_3`)~~ → **fica em
  `SEL_0`**, o default pré-compilado. Mudar o nível exige recompilar o IDF.
  **Consequência a carregar:** a placa reseta numa tensão mais alta que a desejada,
  ou seja, desiste do rail marginal mais cedo — o oposto do "maximizar margem" da
  issue. Candidato número um para a decisão de re-plataforma/bancada.

### Nota para a issue 12

O detector de boot loop da issue 12 esperava consumir o **timestamp do reset**
gravado pelo handler pré-reset desta issue. Como o handler foi removido (Opção 1),
esse timestamp não existe. A issue 12 precisa de outra fonte para "quando foi o
último reset" — `esp_reset_reason()` + o boot counter em NVS são o que há hoje.

### Verificação no target (fila de bancada, não é `/tdd`)

- [ ] Travamento induzido na `flight` → reset em ~5 s (não 3 s), fase e referência
      preservadas em NVS pela persistência contínua da issue 06.
- [ ] Travamento induzido na `io` → reset em ~5 s, arquivo anterior recuperável.
- [ ] Watermarks logados mostram folga confortável nos stacks de 8 KB e 12 KB.
