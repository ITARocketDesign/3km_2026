# ELE3km — Requisitos e Features do Firmware (v2.1)

**Status:** decisões fechadas em entrevista de projeto, 2026-07-28
**Revisado em 2026-07-28** contra `ELE3km_hardware_constraints.md` (que não estava disponível
na primeira redação). Ver §Correções da v2.

**Relação com os outros docs:** este é um **delta**. Onde discordar de
`ELE3km_firmware_PRD.md` ou de `ELE3km_hardware_constraints.md`, **este vence** — ele
incorpora dois requisitos novos (localização inercial como ferramenta explícita, e uso do
SX1276 onboard como segundo transmissor) que revogam decisões daqueles.

Onde **não** discordar, aqueles valem integralmente. Em particular a Parte B (hazards H1–H14)
e a Parte C (arbitragem) do `hardware_constraints` continuam sendo leitura obrigatória; este
documento não as substitui, só as estende.

---

## Correções da v2 (registro honesto)

A primeira versão deste documento foi escrita sem acesso ao `ELE3km_hardware_constraints.md`.
Duas afirmações estavam erradas:

| Afirmação da v2 | Realidade |
|---|---|
| "Recuperação de barramento I²C não está em nenhum doc" | **Está** — hazard **H5**, com mitigação idêntica (9 clocks, STOP, re-init). E a causa raiz do H5 é melhor que a minha: com `U2.GND` flutuando, o MPU6050 é **parasitariamente alimentado pelos diodos de ESD através dos pull-ups de SDA/SCL** — nesse estado ele não deixa de responder, ele segura SDA |
| "Boot counter só no header; precisa ir em cada registro" | **Já era requisito** — mitigação **H4.3**. A *razão* que dei (clusters do voo anterior validando magic+CRC) é diferente e continua válida, e vale documentar junto; mas o requisito não era novo |

Também corrigido: o formato de pacote proposto na v2 (26 B) **furava o teto de duty** — ver
§Formato de pacote.

---

## ⚠️ Revogações

Estas decisões estão **canceladas**. Corrigir antes de escrever código.

| # | O que dizem PRD / connections / constraints | O que passa a valer | Por quê |
|---|---|---|---|
| R1 | *"The onboard SX1276 is never initialized for transmit"* (PRD stories 19, 20; Out of Scope) | **O SX1276 transmite**, em 915–928 MHz, em paralelo ao E22 | Redundância real: regulador diferente. O SX1276 vive no 3V3 interno do Heltec (buck **U7**); o E22 no `3V3_RAIL` (buck **U6**) — o `hardware_constraints` §Power tree marca esse split como significativo (H1, H3) |
| R2 | `PIN_LORA_CS = 18; // DRIVE HIGH FOREVER` (constraints Part A) | GPIO18 é um **chip-select normal**. O passo de boot (HIGH antes de `SPI.begin()`) **continua obrigatório** | H2 continua válido: o pino flutua no reset e o SX1276 se auto-seleciona, corrompendo SD e E22. Só deixa de ser permanente |
| R3 | `PIN_LORA_RST = 14; // hold LOW to keep it dead` | GPIO14 é **liberado** — RESET do SX1276, controlado pelo RadioLib | O rádio precisa funcionar |
| R4 | Regra dura "nenhum write no SD durante TX" (C3 regra 1) | Vale **apenas para o E22**. A airtime do SX1276 **não** bloqueia o SD | A regra protege o **rail U6** (~1 A do PA do E22), não o barramento — o próprio C3 diz isso. O SX1276 não está nesse rail |
| R5 | *"GPIO18 stays HIGH permanently and is not part of the arbitration"* (C3 regra 7) | GPIO18 **entra na arbitragem** como terceiro CS | Consequência direta de R1. Ver §Arbitragem estendida |
| R6 | Estação de solo com um receptor (C5) | **Dois receptores**: 433 MHz (E22) + 915 MHz (SX1276) | Bandas separadas por decisão D2. O requisito de log bruto + timestamp do C5 vale para os dois |

---

## Decisões

### D1 — GPS configurado agressivamente; INS é ponte curta, não navegação

O NEO-6M sai de fábrica no modelo dinâmico **Portable** (assume <1 g). Sob boost de 10–20 g
ele **rejeita a própria solução** e derruba o fix, com reaquisição de 10–60 s — perdendo
subida e parte da descida. Essa é a causa provável da "falha de GPS", e ela é de configuração.

Nenhum dos docs anteriores especifica configuração de GPS. O H3 chega a mandar *"re-apply any
UBX configuration"* na volta de um reset — pressupondo que exista uma. **Esta é ela:**

- No boot: `UBX-CFG-NAV5` com `dynModel = 8` (Airborne <4g)
- Desligar `GSV`, `GSA`, `GLL`, `VTG`; subir para **5 Hz** (`CFG-RATE`, `CFG-PRT`)
- **`UBX-CFG-CFG` salvando em BBR**, para que um reset por sag de rail volte já configurado
- **Reaplicar a configuração inteira** ao detectar reset do GPS (H3 mitigação 2: stream NMEA
  reiniciando, ou satélites caindo a zero logo após um TX). Sem isso o receptor volta em
  **Portable, silenciosamente**, e você perde o fix no boost mesmo tendo configurado
- **Verificar a bateria de backup do breakout** (H3 mitigação 4). Ela serve a dois propósitos:
  warm start em vez de cold start, **e** preservar a configuração UBX salva em BBR
- Limite CoCom (>515 m/s **e** >18 km) **não** se aplica: o foguete faz 222–278 m/s

**Papel do INS:** cobre **apenas dropouts curtos (~10–20 s)**. Passado esse limite, o pacote
carrega **última posição GPS válida + idade em segundos**, não posição inercial derivada.

> Assimetria fundamental do MPU6050 sem magnetômetro: **verticalmente** a integração é
> confiável (a gravidade dá referência de atitude); **horizontalmente** é lixo (heading
> não-observável). É por isso que altitude é o payload prioritário e posição não — e é por
> isso que o INS horizontal não é ferramenta de recuperação, é ferramenta de continuidade.

### D2 — SX1276 em 915–928 MHz

- **Banda: 915–928 MHz.** Não 868 — 863–870 **não é ISM no Brasil** (ANATEL). Metade dos
  exemplos do RadioLib usa `868.0` por default; passaria despercebido
- Bandas separadas ⇒ **sem risco de co-site**. (Em 433 o E22 a +30 dBm entregaria 0 a
  +10 dBm no front-end do SX1276, cujo máximo absoluto é ~+10 dBm)
- Pinos internos ao módulo Heltec: **NSS=18, RST=14, DIO0=26, DIO1=35, DIO2=34**
- **915 MHz atravessa folhagem pior que 433.** O SX1276 é boa redundância **de voo** e beacon
  de recuperação **pior**. O E22 permanece o beacon primário pós-pouso
- **H10 vale para o SX1276 também na parte operacional:** nunca transmitir sem a antena
  conectada. Antena λ/4 ≈ **8 cm** para 915 MHz (a do E22 é ~16,5 cm para 433 — não trocar)

### D3 — Fail-active, não fail-over

Os dois rádios transmitem desde o boot, **sem nenhuma lógica de detecção acoplando-os**.

**Motivo:** sem uplink e sem ACK, o firmware é cego para os modos de falha que importam —
antena solta, PA queimado, rail afundando. RadioLib retorna `RADIOLIB_ERR_NONE` em todos eles.
Um failover disparado por detecção não dispara justamente quando é necessário. Você descobre
a falha **pelos dados, no chão**.

Isso se encaixa na filosofia de camadas do `hardware_constraints` §Parte C: *"nenhuma depende
das outras funcionarem"*. O SX1276 vira uma **quarta camada** independente.

### D4 — Duas tasks FreeRTOS, cores separados

| Task | Core | Prio | Responsabilidade | Barramentos |
|---|---|---|---|---|
| `flight` | 1 | alta | Poll IMU/baro, drena UART do GPS, estimador, decisões | **só I²C e UART** |
| `io` | 0 | média | Dono **exclusivo** do VSPI: SD + E22 + SX1276, arbitragem de CS | **só SPI** |

Ring buffer de 32 KB entre as duas (dimensionamento do C3: pior janela de rádio + stall de
cartão; **inalterado** pela adição do SX1276, que não estende a janela de bloqueio).

**Motivo:** H13 — um microSD **para de responder por 100 ms ou mais** durante garbage
collection. O C3 já pede *"idealmente de uma task separada, fora do loop de amostragem"*;
esta decisão fixa isso como estrutura, não como preferência.

- `startTransmit()` + IRQ, **nunca** `transmit()` bloqueante (já exigido pelo C3)
- **Task watchdog armado nas duas tasks**
- WiFi e Bluetooth desabilitados explicitamente: `WiFi.mode(WIFI_OFF); btStop();`

### D5 — Saúde de módulo: FSM + retry fixo + recovery de barramento

Consolida H3 e H5 numa política única:

- Cada driver expõe `{OK, DEGRADED, FAILED}`; toda operação com **timeout duro**
- Módulo em `FAILED` é retentado a cada **5 s** (período fixo, não backoff exponencial — num
  voo de ~200 s o backoff chegaria no teto em 3 tentativas e viraria a mesma coisa com mais
  estado para testar)
- **Recuperação de barramento I²C (H5):** soltar SDA, pulsar SCL ~9× por bit-bang, STOP,
  re-init. Sem ela, `reinit()` a cada 5 s falha para sempre — o escravo continua segurando a
  linha
- **Ordem de probe importa (H5.3): detectar o barômetro *primeiro*.** Se o probe da IMU travar
  o barramento, você pelo menos sabe que o baro estava saudável
- **Re-verificação periódica dos registradores de *config*** (H3.1), no caminho `OK`. Um
  BMP280 ou MPU6050 que fez brown-out e voltou responde ACK, tem `WHO_AM_I`/`CHIP_ID` corretos,
  e devolve dados plausíveis em modo sleep / escala errada. Só a config denuncia
- **Contador de reinit por sensor logado** (H3.3): se subir durante o voo, o rail está
  afundando e a rev.2 precisa de um buck separado para o rádio
- **Orçamento de tempo no retry:** `Wire.setTimeOut()` explícito e baixo; no máximo **um**
  módulo tentado por ciclo
- Estado de saúde de cada subsistema entra em **todo log record e todo pacote**

> H9: ficar em **100 kHz** no I²C. Não tentar 400 kHz. GPIO21 é também o controle de Vext do
> Heltec e a rede do gate é carga extra no SDA.

### D6 / D7 — Pacote idêntico nos dois rádios, 1 Hz cada

- **Mesmo codec, mesmo payload, mesmas duas formas** (full e altitude-only) nos dois caminhos
- **Ambos a 1 Hz**, **seq global único**, **defasados ~500 ms**
- Custo do SX1276: 20 B em SF7/BW125 ≈ **46 ms** ⇒ **4,6% de duty**, ~5,5 mA médios. Contra o
  orçamento do C6 (≈200 mA totais em FLIGHT), é ruído — a autonomia de ~5 h não muda
- **Ganho: diversidade de recepção.** Os dois links falham por motivos independentes (banda,
  antena, polarização durante o rolamento, multipath descorrelacionado). A probabilidade de
  perder um pacote vira o *produto*, não o mínimo. A estação de solo dedupa por seq

**A defasagem de 500 ms tem duas justificativas, não uma:**
1. Não empilhar as duas transmissões na mesma janela da task de I/O, que também escreve no SD
2. **H3, nota final:** os dois bucks compartilham o nó da bateria. Sobrepor o TX do SX1276
   (~54 mA do pack) ao do E22 (~0,5 A do pack) soma carga num LiPo pequeno com resistência
   interna alta. A 500 ms de defasagem, o SX1276 termina em t≈546 ms e o E22 só rekeya em
   t=1000 ms — **zero sobreposição**

**Pós-pouso os dois divergem:** E22 vai a SF12 / 20 s (C1 — beacon de mato, 433 penetra
melhor); SX1276 mantém ~1 / 5 s.

### D8 — Ciclo de 100 Hz

| Item | Taxa | Custo |
|---|---|---|
| Ciclo / Δt do Kalman / log | **100 Hz** (Δt = 10 ms) | 6,4 kB/s |
| MPU6050 | 100 Hz | 14 B @ 100 kHz ≈ 1,5 ms ⇒ **15% do I²C** |
| BMP280 | 25 Hz | ≈ **2% do I²C** |
| GPS | 5 Hz | UART |

Total ~17% do barramento, com folga para retries e recovery. A 200 Hz seriam ~32%, num
barramento que o H9 já marca como frágil.

- Bate com o C3: *"a 64-byte record at 100 Hz is 6.4 KB/s"*. Buffer de **32 KB** confirmado
- ~1,3 MB para 200 s de voo. **Pré-alocar 64 MB**
- **Depois do pouso confirmado, log cai para 1 Hz** — senão o beacon loga por horas a 6,4 kB/s

Mantido integralmente do C4: `SdFat` (não a lib `SD`), arquivo pré-alocado e contíguo, FAT não
tocada durante o voo, metadados escritos duas vezes, arquivo novo por boot, blocos alinhados
em 512 B, timestamp na **aquisição** e nunca na escrita.

### D9 — Cadeia de altitude e a porta estática

O H12 nomeia o problema (*"a bare BMP280 on a breakout, **without a properly designed static
port**, will read badly wrong pressure"*) mas não dimensiona a solução. Esta é ela.

**A conta que assusta.** A 278 m/s, `q = ½ρv² ≈ 46 kPa` contra ~100 kPa de estática. **5% de
pressão de estagnação vazando = 2,3 kPa ≈ 200 m de altitude fantasma.** Bay selado é pior de
outro jeito: vira termômetro, reporta subida enquanto a eletrônica esquenta na rampa.

**Dimensionamento** — erro de atraso em regime permanente:

```
ṁ_req = V·ρ·g·v / (R·T)        ΔP = ṁ_req² / (2ρ(Cd·A)²)        Δh = ΔP / (ρg)
```

Com bay de 300 cm³, v = 278 m/s, ρ = 1,2, Cd = 0,6, **3 furos de 2 mm** (A = 9,4 mm²):
ΔP ≈ **1,9 Pa** ⇒ Δh ≈ **0,16 m**.

**Conclusão contra-intuitiva: não dá para fazer os furos pequenos demais para o atraso, e
pequeno é melhor** (furo grande deixa turbulência bombear o bay). Além disso o erro é
**proporcional à velocidade**, e no apogeu a velocidade é zero — **o número que a competição
pontua sai praticamente livre de atraso**, mesmo com porta pequena.

**Receita:** 3 ou 4 furos de ~2 mm, **igualmente espaçados na circunferência** (cancela erro de
ângulo de ataque e bombeamento por rolamento — esta é a parte que realmente conta), ≥1 diâmetro
de corpo atrás de qualquer mudança de geometria e ≥1 diâmetro à frente das aletas, sem rebarba,
furo perpendicular à pele.

**Firmware** (mantém H12 e detalha): canal vertical do Kalman com **R do barômetro variável em
função da velocidade estimada** — peso alto abaixo de ~30 m/s (rampa, apogeu, descida, pouso),
peso quase nulo acima de ~100 m/s. No boost a altitude vem da integração vertical do
acelerômetro. Mais: rejeitar degraus impossíveis de pressão por limite de taxa (H12.2), IIR
interno do BMP280 ligado ciente do lag que ele adiciona (H12.3), pressão bruta logada ao lado
da altitude derivada (H12.4).

Mantido do H11: amostras da IMU em ±16 g / ±2000 °/s são **flagadas como saturadas** e
excluídas (ou com covariância de processo inflada) no predict step; escala configurada em
±16 g / ±2000 °/s desde o início; **DLPF ajustado** para que a vibração do motor não faça
aliasing no acelerômetro.

### D10 — Posição de recuperação

Depois do pouso a situação **inverte**, e nenhum doc trata isso.

- **Reconfigurar o GPS para `dynModel = 2` (Stationary)** ao entrar em `LANDED`. O modelo
  Airborne da D1 diz ao receptor "espere aceleração" e **impede** a filtragem pesada que dá
  ~2 m parado
- **Acumular apenas fixes bons** (filtro por nº de satélites e HDOP) e transmitir a **média
  acumulada** — não o último fix, que sob copa pode ser o pior
- **Qualidade no pacote:** nº de amostras na média, HDOP, idade. Senão a equipe de busca não
  sabe se caminha para um ponto ou para um círculo de 100 m
- **Sem fix nenhum:** transmitir o **último fix válido de voo** + altitude barométrica da
  descida. Um fix a 200 m na descida limita o ponto de pouso a um raio pequeno

### D11 — Costura de teste

**Regra: a task de I/O não contém política nenhuma.** É uma bomba burra.

`core/` (puro — **zero `Arduino.h`, zero `millis()`, zero global**; o tempo entra como
parâmetro) contém:

- Estimador (Kalman: horizontal pos+vel, vertical alt+vz)
- Máquina de estados de fonte de posição (GPS ↔ INS)
- Máquina de fases de voo
- Codec de telemetria
- **Escalonador de arbitragem** — recebe "estado + tempo", devolve "próxima ação". Aqui vivem:
  a regra `E22 ⊕ SD_write`, a política de descarte no overflow, e qual rádio transmite quando

Sem isso, as invariantes do C3 (regras 1, 2, 6) e os testes que o PRD exige delas viram código
que só é exercido voando.

**Harness de replay — o item de maior valor da suíte.** O `LogRecord` de 64 B carrega os
valores **brutos** *e* os fundidos ⇒ um log de voo real pode ser reproduzido pelo core no host.
**O log do primeiro voo vira o teste de regressão de todas as versões futuras do estimador.**

> **Requisito escrito:** o `LogRecord` deve ser suficiente para reconstruir o `SensorSample`
> que o produziu. Nenhum campo bruto pode ser removido sob o argumento de que "já está no
> fundido".

`platformio.ini` precisa de um `[env:native]` com Unity, além do env de target.

**Teste de maior valor** (C2): perfil sintético de voo completo (rampa → boost → coast →
**queda livre no apogeu** → descida → toque) verificando que a fase permanece `FLIGHT` no
apogeu. Queda livre lê **≈0 g de forma estável**; em repouso lê **≈1 g**. Essa distinção é o
teste inteiro.

### D12 — Referência barométrica de solo

O H4 manda persistir e reusar, mas deixa três buracos: **como decidir que o reset foi
inesperado**, **quando tirar a referência**, e **como detectar liftoff** (o C2 referencia
"liftoff detection" sem nunca defini-la). Esta decisão fecha os três.

- Enquanto a fase é `PAD`: **média lenta contínua** da pressão (constante de tempo de dezenas
  de segundos) + anel curto de histórico. Referência tirada no power-on está velha na hora da
  ignição: a atmosfera deriva, a eletrônica esquenta, e o erro entra **direto no apogeu**
- **Liftoff** por **|a| > ~2,5 g sustentado por ~100 ms** — aceleração, não altitude (a
  altitude é justamente o que ainda não se pode medir)
- No liftoff a referência é **congelada no valor de ~1 s antes**, lido do anel: evita
  contaminação pelo transiente do motor e por manuseio na rampa
- Persistida em **NVS** nesse instante e nas transições de fase — ~5 escritas por voo, não 100
  por segundo (o que destruiria o flash em minutos)
- **Na volta de um reset:** reusa a referência salva **apenas se** `esp_reset_reason() !=
  ESP_RST_POWERON` **e** a fase persistida era `FLIGHT`. Sem o segundo teste, um brownout na
  rampa faz o foguete voar com referência velha
- Mantida a sanidade do H4.4: se a referência reusada implicar altitude absurda, cair para
  altitude do GPS e **flagar no pacote**

### D13 — Regras de disciplina (absorvidas do `hardware_constraints`)

Custam zero código e são as mais prováveis de queimar um dia de debug ou um módulo. **Primeiro
commit.**

| Regra | Origem | Consequência de violar |
|---|---|---|
| **Nenhum código de LED em lugar nenhum.** GPIO25 pertence ao RadioLib | H8 | Piscar o LED do Heltec **keya o PA de 1 W** |
| **Só o RadioLib toca GPIO12 e GPIO25**, via `setRfSwitchPins(12, 25)`. Nunca `digitalWrite()` direto | H10 | TXEN+RXEN altos simultaneamente roteia saída do PA para o LNA — **módulo destruído** |
| **Nunca dirigir GPIO12 alto antes do boot terminar.** Não montar pull-up externo nele | H7 | Strapping MTDI = tensão do flash. A placa **não dá boot** |
| **Não usar ADC1, sensor Hall, touch, nem ULP.** Nada de `analogRead()`/`hallRead()`/`touchRead()` | H6.1 | Errata do ESP32: GPIO36/39 registram pulso baixo espúrio quando o domínio SAR/ADC chaveia ⇒ IRQ fantasma do rádio |
| **Nunca confiar numa borda de DIO1 sozinha** — ler o registrador de IRQ status do rádio e ignorar se nenhum flag estiver setado (`getIrqStatus()`) | H6.2 | "Radio ready" falso |
| **Nunca esperar em BUSY indefinidamente** — timeout limitado + caminho de recuperação (pulso NRST, re-init) | H6.3 | Travamento permanente da task de I/O |
| **Não cortar o VCC do E22 entre transmissões** | C3.8 | Não há load switch na placa; standby já é ~2 mA; NRST não tem pull-up |
| **Nunca transmitir sem antena conectada** (vale para os dois rádios) | H10 | 1 W em circuito aberto **destrói o PA** |

---

## Arbitragem estendida — onde o SX1276 entra no ciclo do C3

O C3 define `RADIO_TX → SD_WRITE → IDLE` com **GPIO18 fora da arbitragem** (regra 7). Isso é
revogado por R5. O modelo estendido:

**O recurso escasso continua sendo o rail U6, não o barramento.** O SX1276 não está no U6.
Então ele entra no ciclo pelo **barramento**, não pelo rail:

| Recurso | Quem disputa |
|---|---|
| **Rail U6** (≈1 A) | E22 keyed **⊕** write no SD. Regra dura, inalterada |
| **Barramento VSPI** | SD **⊕** E22 (carga de payload) **⊕** SX1276 (carga de payload). Três CS: 18, 23, 32 — exatamente um baixo por vez |
| **Nó da bateria** | TX do E22 **⊕** TX do SX1276 (H3) — resolvido pela defasagem de 500 ms, não por travamento |

Consequências concretas:

1. A **airtime** do SX1276 (46 ms) **não bloqueia nada** — nem SD, nem E22. Ele transmite
   autonomamente enquanto o barramento está livre e o rail U6 intocado
2. A **carga de payload** do SX1276 é uma transação curta de barramento (~2 ms). Ela é
   agendada em `IDLE`, entre blocos de escrita do SD
3. A defasagem de 500 ms garante que a airtime do SX1276 (t≈500–546 ms) termine bem antes do
   E22 rekeyar (t=1000 ms). **Não é uma trava, é um agendamento** — mas o escalonador puro
   (D11) deve verificar a invariante, não presumi-la
4. Os chip-selects continuam sendo assertados **apenas pelo dono corrente, dentro de transações
   SPI** (C3.7, parte que sobrevive). O que muda é que GPIO18 agora tem um dono

---

## Formato de pacote (proposta — contrato com a estação de solo)

**Correção sobre a v2: airtime LoRa é quantizada em blocos de 8 símbolos.** Em SF8/BW125 isso
é **16,4 ms por degrau**. Rodando a fórmula de Semtech contra a tabela do H1 (20 B → 68,25
símbolos → 139,8 ms, batendo com os 140 ms tabelados), o teto do bucket de 140 ms é **21
bytes**. A proposta anterior de 26 B custava **156 ms** e furava o teto de duty do C6.

**O CRC16 de aplicação foi removido do pacote de rádio** — o LoRa já tem CRC de hardware
(`CRC on` no C1) e o RadioLib não entrega pacote reprovado. No `LogRecord` do SD o CRC
**permanece**: lá não existe camada de enlace.

Little-endian (nativo do ESP32), layout fixo.

**Full — 20 B exatos ⇒ 140 ms em SF8, mantendo os 14% de duty do C1/C6 sem alteração**

| Off | Tipo | Campo |
|---|---|---|
| 0 | `u8` | magic (4 bits) + versão (4 bits) |
| 1 | `u16` | seq — global, único, compartilhado pelos dois rádios |
| 3 | `u16` | t — decissegundos desde o liftoff (0…6553 s) |
| 5 | `i32` | lat × 1e7 |
| 9 | `i32` | lon × 1e7 |
| 13 | `i16` | altitude, m acima da referência |
| 15 | `i16` | velocidade vertical, dm/s (±327 m/s — cobre os 278 m/s) |
| 17 | `u8` | flags: fase (3b) + fonte de posição (2b) + qualidade de fix (3b) |
| 18 | `u8` | saúde: bitmap {imu, baro, gps, sd, e22, sx1276} |
| 19 | `u8` | GPS: sats (4b) + HDOP (4b) |

**Altitude-only — 12 B** (idêntico sem `lat`/`lon`) ⇒ **107 ms** em SF8.
Altitude presente nas **duas** formas.

Em SF12 (beacon pós-pouso): 20 B ⇒ 1712 ms, exatamente o valor tabelado no C1. **Nada no
orçamento do C6 muda.**

---

## Pendências e ações

| # | Ação | Dono |
|---|---|---|
| A1 | **Porta estática:** 3–4 furos de 2 mm, simétricos, posicionados conforme D9 | Mecânica / fuselagem |
| A2 | **Jumper de GND do MPU6050** (U2.GND é rede de um pino só). Enquanto não existir, o H5 está ativo: a IMU pode **travar o I²C e levar o barômetro junto** | Montagem |
| A3 | Confirmar o tipo do módulo microSD: precisa ser o **"Micro SD Storage Board" 3,3 V**, não o adaptador com AMS1117 + buffer (espera 5 V e dirige MISO com CS alto) | Montagem |
| A4 | Trimpots: **U6 = 3,30 V**, **U7 = 5,0 V**, sem carga, antes de plugar os módulos | Montagem |
| A5 | Antena λ/4 de **~8 cm** (915 MHz) para o SX1276. Não confundir com a de 16,5 cm do E22 | Montagem |
| A6 | Verificar a **bateria de backup do breakout do GPS** — ela guarda a config UBX em BBR *e* dá warm start (D1, H3.4) | Montagem |
| A7 | **Dois receptores** na estação de solo (433 + 915), cada um com log bruto + timestamp de todo pacote (C5) | Estação de solo |
| A8 | Confirmar o formato de pacote acima antes de congelar o codec | Firmware + solo |
| A9 | `E22_integration.md` continua ausente do `Docs/`. Os três docs presentes já marcam seus dois itens obsoletos (`setRfSwitchPins(2,25)` e o `R3` nunca montado), então **não é bloqueante** — mas confirmar que não há mais nada nele | Documentação |

---

## Contrato de pinos — delta

Vale a Parte A do `ELE3km_hardware_constraints.md`, **com estas mudanças**:

```cpp
// SX1276 onboard — AGORA ATIVO (revoga R2/R3)
constexpr int PIN_LORA_CS   = 18;   // chip-select normal; HIGH antes de SPI.begin()
constexpr int PIN_LORA_RST  = 14;   // liberado — RESET, controlado pelo RadioLib
constexpr int PIN_LORA_DIO0 = 26;
constexpr int PIN_LORA_DIO1 = 35;   // input-only
```

**Ordem de boot — inalterada e obrigatória** (H2, H14):

```cpp
pinMode(PIN_LORA_CS, OUTPUT);  digitalWrite(PIN_LORA_CS, HIGH);  // 1. deselecionar SX1276
pinMode(PIN_SD_CS, OUTPUT);    digitalWrite(PIN_SD_CS, HIGH);    // 2. deselecionar SD
pinMode(PIN_E22_NSS, OUTPUT);  digitalWrite(PIN_E22_NSS, HIGH);  // 3. deselecionar E22
// PIN_LORA_RST NÃO é mais forçado LOW — pertence ao RadioLib (R3)
// GPIO12 NÃO é tocado aqui — strapping, R2 mantém LOW em hardware (H7)
// só agora: SPI.begin(5, 19, 27), depois os drivers
```

**Três chip-selects vivos no VSPI: 18, 23, 32.** Exatamente um baixo por vez, e nunca todos
altos-Z durante o bootloader (H14).
