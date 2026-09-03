# Issues — Firmware ELE3km

16 fatias verticais derivadas de `Docs/ELE3km_firmware_PRD_v3.md`, com `ELE3km_connections.md` (netlist) e `ELE3km_hardware_constraints.md` (hazards H1–H16, arbitragem C1–C6) como leitura obrigatória antes de escrever código.

No repositório do firmware há mais dois documentos que valem antes de mexer em código: `ELE3km/DISCIPLINE.md` (as oito regras de §12, cinco delas custam hardware) e `ELE3km/PACKET_FORMAT.md` (o formato no ar, que é contrato com a estação de solo).

O terceiro contrato do projeto **não** tem documento em prosa: o formato do log no cartão mora no cabeçalho de `ELE3km/src/core/log_codec.h`, com o layout de 64 B campo a campo e o porquê de cada decisão. Ele é contrato com `tools/recover_log` e com o harness de replay da issue 14 — se essa issue quiser um `LOG_FORMAT.md` no mesmo espírito do de pacote, o material já está escrito, só não está em prosa.

Cada issue implementada carrega uma seção **Estado da implementação** no fim do próprio arquivo, com o que ficou onde, as decisões que divergem do texto da issue, e o que a fatia seguinte herda. É por lá que se começa uma sessão nova.

**Substitui** o conjunto anterior de 17 issues derivadas da v2, que era organizado por camada (codecs, FSMs, HALs, integração). Nada chegou a ser implementado contra aquele conjunto: quando ele foi descartado, `src/main.cpp` ainda era o boilerplate do PlatformIO.

## Corte vertical, não horizontal

Cada issue é uma **fatia fina que atravessa todas as camadas** — build, HAL, núcleo puro, codec, rádio — e termina em comportamento observável na placa ou numa suíte nativa que passa. A issue 01 já põe altitude no ar; cada issue seguinte engrossa um caminho que já funciona.

O custo dessa escolha é paralelismo: o conjunto por camada permitia doze issues simultâneas depois do scaffold, e este é mais encadeado. A contrapartida é que não existe um "big bang" de integração no fim, e a qualquer momento existe um firmware que voa.

## Grafo de dependências

```
01 Tracer: altitude no ar (SX1276)
├── 02 GPS e pacote completo 20 B
│   ├── 03 E22 fail-active + escalonador
│   │   ├── 04 Log microSD + recuperação
│   │   │   ├── 05 Tasks FreeRTOS + ring buffer SPSC
│   │   │   │   └── 11 Watchdog + stack + brown-out
│   │   │   │       └── 12 Boot loop + beacon ◄── (03)
│   │   │   ├── 06 Fases + referência barométrica ──┐
│   │   │   ├── 10 Saúde e degradação ◄── (09)      │
│   │   │   └── 14 Harness de replay ◄── (07)       │
│   │   └────────────────────────────────────┐     │
│   ├── 08 Fonte de posição ◄── (07) ────────┤     │
│   └── 16 HITL: contrato estação de solo    │     │
│                                            └── 13 Pós-pouso
├── 07 Estimador Kalman + sanidade
│   └── 09 Recuperação de barramento I²C
│
15 HITL: porta estática (independente)
```

## Lista de issues

| # | Título | Tipo | Bloqueada por | Status |
|---|--------|------|---------------|--------|
| [01](01-tracer-altitude-sx1276.md) | Tracer bullet: altitude no ar pelo SX1276 | AFK | — | Implementada — falta verificar no target |
| [02](02-gps-pacote-completo.md) | Posição GPS e pacote completo de 20 B | AFK | 01 | Implementada — falta verificar no target |
| [03](03-e22-fail-active-escalonador.md) | Segundo rádio: E22 fail-active e escalonador | AFK | 02 | Implementada — falta verificar no target |
| [04](04-log-microsd.md) | Log durável no microSD e recuperação pós-voo | AFK | 03 | Implementada — falta verificar no target |
| [05](05-tasks-freertos-ring-buffer.md) | Duas tasks FreeRTOS e ring buffer SPSC | AFK | 04 | Implementada — falta verificar no target |
| [06](06-fases-referencia-barometrica.md) | Fases de voo e referência barométrica | AFK | 04 | Implementada — falta verificar no target |
| [07](07-estimador-kalman-sanidade.md) | Estimador Kalman com sanidade numérica | AFK | 01 | Implementada — falta verificar no target |
| [08](08-fonte-de-posicao.md) | Fonte de posição: GPS ↔ INS ↔ última válida | AFK | 02, 07 | Implementada — falta verificar no target |
| [09](09-recuperacao-barramento-i2c.md) | Recuperação do barramento I²C e ordem de probe | AFK | 07 | Implementada — falta verificar no target |
| [10](10-saude-e-degradacao.md) | Máquina de saúde e degradação por subsistema | AFK | 04, 09 | Implementada (fatia de núcleo) — falta verificar no target |
| [11](11-watchdog-stack-brownout.md) | Watchdog, stack e brown-out | AFK | 05 | Implementada (Opção 1) — falta verificar no target |
| [12](12-boot-loop-beacon-sobrevivencia.md) | Detector de boot loop e beacon de sobrevivência | AFK | 03, 11 | Implementada — falta verificar no target |
| [13](13-comportamento-pos-pouso.md) | Pós-pouso: Stationary, média filtrada, beacon | AFK | 03, 06, 08 | Implementada (fatia de núcleo) — falta verificar no target |
| [14](14-harness-replay.md) | Harness de replay | AFK | 04, 07 | Implementada (host/native, sem bancada) |
| [15](15-hitl-porta-estatica.md) | Especificação da porta estática | **HITL** | — | |
| [16](16-hitl-contrato-estacao-solo.md) | Contrato imposto à estação de solo | **HITL** | 02 | |

## Onde estamos

**As 14 issues de código (01–14) estão implementadas.** As suítes nativas passam
(102 casos em doze suítes), o build do target compila, e os cinco greps de
`ELE3km/DISCIPLINE.md` saem vazios. **A fila `/tdd` está vazia.** 01–13 têm a mesma
pendência — verificação no target, na fila de bancada abaixo; a **14** é host/native
e não tem bancada. O que resta é bancada, as duas HITL (15, 16), e a decisão de
contrato adiada pela 14 (levar `gps.altitude_m` e `accel_saturated` ao formato do
cartão, para reproduzir voos reais bit a bit).

As pendências de target se acumularam e vale montar a bancada uma vez só:

| Issue | O que falta ver | O que precisa |
|---|---|---|
| 01, 02 | Altitude e posição no ar | placa + um receptor de 915 MHz |
| 03 | Mesma sequência nas duas bandas, defasada ~500 ms | **as duas antenas** + um receptor de 915 MHz (SX1276) + um de 433 MHz (E22) |
| 04 | Arquivo gravado e recuperado | cartão microSD |
| 05 | Travada induzida de ~250 ms no cartão não abre lacuna nas amostras | placa + cartão (de preferência um lento) |
| 06 | Reset de bancada em "voo" reusa a referência salva em NVS | placa (agora com a IMU da 07, a fase detecta liftoff de verdade) |
| 07 | A IMU real alimenta o canal inercial; liftoff dispara por \|a\| | placa + MPU6050 (a bancada pode induzir ~2,5 g à mão) |
| 08 | Vão real de GPS produz GPS → INS → última válida | placa + GPS |
| 09 | SDA presa destrava; IMU removida e o baro segue no ar; recuperação não estoura o ciclo | placa + MPU6050 + jumper para segurar SDA |
| 10 | `verify_config()` relê os registradores de BMP280/MPU6050; cadência de recuperação I²C de 5 s; contadores de reinit no Serial | placa + MPU6050 |
| 11 | Watchdog reseta as duas tasks; watermarks têm folga | placa + cartão microSD |
| 12 | Três resets ativam SX1276/SF12 sozinho; power-on reabilita E22 | placa + **as duas antenas** + um receptor de 915 MHz (SX1276) + um de 433 MHz (E22) |
| 13 | GPS→Stationary ao pousar; E22 SF12 a 1/20 s e SX1276 a ~1/5 s (defasagem mantida); log a 1 Hz; beacon roda por horas | placa + **as duas antenas** + um receptor de 915 MHz (SX1276) + um de 433 MHz (E22) + GPS |

⚠️ A partir da issue 03 a placa aciona um PA de 1 W. **Nunca ligar sem as duas
antenas conectadas** — o firmware não detecta antena solta, e 1 W em circuito
aberto destrói o PA.

Com a 07 fechada, o ramo do estimador destravou **08, 09 e 14**; fechadas a **08**,
a **09**, a **10** e a **13**, a única issue de código que resta é a **14**:

- **08** — fonte de posição (GPS ↔ INS ↔ última válida). **Feita.** A máquina de
  quatro estados vive no `core/`, pura; a idade do fix vai ao log (não ao ar) e a
  fonte LastValid sinaliza posição velha. A reconciliação de referencial da altitude
  do GPS ficou de fora: os critérios da 08 não a pedem.
- **09** — recuperação do barramento I²C. **Feita.** O mecanismo (`hal/i2c_bus`,
  a rotina de clock-out do H5) e a guarda de um módulo por ciclo na task de voo; o
  carimbo durável no cartão e a cadência de 5 s ficaram para a 10, que chama a
  rotina como primitiva.
- **10** — saúde e degradação. **Feita** (fatia de núcleo). Máquina
  `{OK, DEGRADED, FAILED}`, retry de 5 s, contador de reinit e o bitmap de 6 bits no
  pacote e no registro. A adoção por driver (`verify_config()`, cadência I²C de 5 s)
  foi para a bancada.
- **13** — pós-pouso. **Feita** (fatia de núcleo). Filtro e média de fixes pós-pouso
  (sat ≥ 4, HDOP ≤ 5,0), amostras nos 3 bits do byte 17 por fase, e log a 1 Hz em
  POUSADO. GPS→Stationary, a cadência divergente dos rádios e o SF físico foram para
  a bancada.
- **14** — harness de replay. **Feita** (host/native). Reconstrói a amostra dos
  campos brutos, reproduz pela `FlightComputer` e compara com tolerância declarada;
  ciclo sintético fecha com divergência zero; `tools/replay` + `tools/REPLAY.md`.
  Descobriu que o registro de 64 B não carrega `gps.altitude_m` nem
  `accel_saturated` — voo real não reproduz bit a bit; as duas viraram lacunas
  documentadas e a mudança do formato ficou como decisão de contrato à parte.
- **15** e **16** — as duas HITL. A 15 é independente de tudo e tem prazo de
  fabricação próprio; a 16 tem `ELE3km/PACKET_FORMAT.md` como insumo pronto — e agora
  também precisa do overload dos bits 5–7 do byte 17 em POUSADO (issue 13).

## Onde há paralelismo

- **15** é independente de tudo e tem prazo de fabricação próprio — começar já.
- **07** (o outro ramo longo, solto desde a 01) está feita: abriu **08, 09 e 14**,
  todas fechadas.
- Depois de **04**: **05** e **06** eram independentes entre si — ambas feitas.
- Depois de **02**: **16** pode ser negociado com a equipe de solo enquanto o firmware avança.
- Caminho crítico: **01 → 02 → 03 → 04 → 05 → 11 → 12**. Implementação completa;
  faltam as verificações de bancada acumuladas.

## Como verificar

O `pio` não está no PATH; ele vive no ambiente virtual do PlatformIO.

```bash
cd ELE3km && ~/.platformio/penv/bin/pio test -e native
```

```bash
cd ELE3km && ~/.platformio/penv/bin/pio run -e heltec_wifi_lora_32_V2
```

```bash
make -C tools && tools/recover_log FLIGHT007.BIN > voo7.csv
```

```bash
make -C tools replay && tools/replay FLIGHT007.BIN   # harness de replay (issue 14)
```

Além desses três, os cinco greps do fim de `ELE3km/DISCIPLINE.md` **têm que sair
vazios** — eles são a parte mecanicamente verificável das regras que custam
hardware, e um deles já foi quebrado uma vez por um comentário que mencionava a
palavra que o grep procura.

## Marcos verificáveis

| Depois de | O que a placa faz |
|---|---|
| 01 | Transmite altitude a 1 Hz em 915 MHz |
| 02 | Transmite posição quando há fix, altitude sempre; sobrevive a um reset do receptor |
| 03 | Dois rádios no ar, mesma sequência, defasados 500 ms |
| 04 | Grava o voo no cartão, recuperável mesmo truncado, sem escrever com o PA de 1 W no ar |
| 06 | Detecta liftoff e pouso; sobrevive a apogeu sem falso pouso |
| 08 | Degrada honestamente quando o GPS cai |
| 12 | Sobrevive a rail marginal sem ficar mudo |
| 13 | Beacon de recuperação com posição média e qualidade declarada |

## Invariantes que atravessam todas as issues

Estas não pertencem a uma issue, valem para todas, e a maioria custa hardware se violada:

- **Nenhum código de LED em lugar nenhum.** O pino do LED do módulo é o habilitador de transmissão — piscar o LED liga o PA de 1 W.
- **Só a biblioteca de rádio toca os pinos do comutador de RF.** Dois habilitadores altos ao mesmo tempo roteiam a saída do PA para o LNA e destroem o módulo.
- **Nunca transmitir sem antena conectada**, nos dois rádios.
- **Nunca confiar numa borda de IRQ sozinha** — sempre confirmar contra o registrador de status (errata do MCU).
- **Nunca esperar indefinidamente** no sinal de ocupado: timeout mais caminho de recuperação.
- **Não usar ADC1, sensor Hall, touch nem coprocessador de baixo consumo.**
- **Nunca acionar o pino de strapping antes do boot terminar.**
- **Não cortar a alimentação do rádio externo entre transmissões.**
- O `core/` não tem **nenhum header de Arduino, nenhum relógio global, nenhuma variável global**. O tempo entra como parâmetro.
- O mapa de pinos vive em **exatamente um lugar** e vem do netlist.
- Nenhum teste inspeciona estado interno — só comportamento externo do núcleo.
