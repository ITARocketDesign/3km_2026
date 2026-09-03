# 06 — Fases de voo e referência barométrica

**Tipo:** AFK
**User stories:** 4, 17, 52, 53, 54, 55, 56, 57, 58, 59, 60

## What to build

A máquina de fases RAMPA → VOO → POUSADO no `core/` puro, com a referência barométrica que dá sentido à altitude transmitida. A partir desta fatia o campo `t` do pacote passa a ser decissegundos desde o liftoff.

**Referência na rampa:** média lenta contínua da pressão ambiente, com constante de tempo de dezenas de segundos, mais um anel curto de histórico. Uma referência tirada no power-on já está velha na hora da ignição — a pressão local muda, e o foguete fica na rampa por minutos.

**Liftoff por aceleração**, |a| acima de um limiar sustentado por ~100 ms. **Não por altitude** — a altitude é justamente o que ainda não se pode medir sem referência congelada.

**No liftoff a referência é congelada no valor de ~1 s antes**, lido do anel de histórico, para não contaminá-la com o transiente do motor nem com manuseio na rampa.

**Persistência em NVS** no instante do liftoff e nas transições de fase — poucas escritas por voo, para não destruir o flash.

**Ao voltar de um reset:** reusa a referência salva **apenas se** o motivo do reset não foi um power-on limpo **e** a fase salva era de voo. Um brown-out na rampa não pode fazer o foguete voar com referência velha. Sanidade: se a referência reusada implicar uma altitude absurda, cai para altitude de GPS e **sinaliza isso no pacote**.

**POUSADO exige quatro condições simultâneas:**

1. Aceleração perto de 1 g
2. Altitude estável numa faixa por N segundos
3. Altitude perto da referência de solo
4. Tempo mínimo desde o liftoff

**Por que quatro e não uma:** em queda livre no apogeu o acelerômetro lê ≈0 g **de forma estável**; em repouso lê ≈1 g. Um detector baseado em baixa variância dispara no apogeu e joga fora a telemetria da descida inteira — que é exatamente a razão de existir deste projeto. A distinção 0 g estável vs. 1 g é o teste inteiro.

**A transição para POUSADO é de mão única.** Só um reset sai dela.

A fase é registrada em todo registro de log e em todo pacote.

## Acceptance criteria

- [x] `core/` com máquina de fases RAMPA → VOO → POUSADO, pura, tempo como parâmetro
- [x] `core/` com a média lenta de pressão na rampa e o anel de histórico
- [x] Detecção de liftoff por aceleração sustentada acima de um limiar por ~100 ms
- [x] No liftoff a referência congelada é o valor de ~1 s antes, lido do anel
- [x] Referência, fase e contador de boot persistidos em NVS no liftoff e nas transições de fase — *snapshot passado da task flight para a task io, que grava; ver ressalva*
- [x] Reuso da referência salva **apenas se** o reset não foi power-on limpo **e** a fase salva era de voo
- [x] Referência reusada que implique altitude absurda → fallback para altitude de GPS, sinalizado no pacote
- [x] POUSADO só é declarado com as quatro condições simultâneas
- [x] POUSADO é de mão única — nenhuma transição de saída
- [x] Fase presente em todo registro de log e em todo pacote
- [x] Campo `t` do pacote passa a ser decissegundos desde o liftoff
- [x] **Teste nativo de maior valor da suíte:** perfil sintético completo (rampa → boost → coast → queda livre no apogeu → descida → toque) e a fase permanece em VOO durante toda a queda livre do apogeu
- [x] Teste nativo: os pacotes continuam na cadência de voo durante toda a descida
- [x] Teste nativo: POUSADO nunca é abandonado depois de atingido
- [x] Teste nativo: reset simulado em pleno voo reusa a referência salva e a altitude reportada fica contínua
- [x] Teste nativo: power-on limpo simulado **não** reusa a referência
- [x] Teste nativo: no liftoff a referência congelada é a de ~1 s antes, não a do instante do transiente
- [x] Teste nativo: referência reusada absurda cai para GPS e o pacote sinaliza

## Blocked by

- Issue 04 (log e NVS — a persistência da fase e da referência precisa de NVS)

## Estado da implementação

Tudo implementado menos o critério de target implícito (persistência real em NVS na
volta de um reset de bancada), que precisa de placa. `pio test -e native` passa com
57 casos em cinco suítes — 11 novos na suíte `test_phase`, mais dois no seam do
FlightComputer e um de altitude no `test_nmea`. `pio run -e heltec_wifi_lora_32_V2`
compila (RAM 26,0 %), e os cinco greps de `DISCIPLINE.md` saem vazios.

O teste de maior valor da suíte está lá: um perfil sintético de rampa → boost →
coast → queda livre no apogeu → descida → toque, em que a fase permanece em VOO
durante toda a queda livre (0 g estável) e só vira POUSADO no repouso a 1 g. É o
teste que pega um detector de pouso construído sobre baixa variância.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/flight_phase.{h,cpp}` | A máquina de fases, a média lenta, o anel de histórico, liftoff, pouso e a decisão de reuso |
| `src/core/flight_computer.{h,cpp}` | Passou a compor o `PhaseEstimator`; fase no log e no pacote, `t` desde o liftoff |
| `src/hal/flight_state.{h,cpp}` | NVS: grava/lê o snapshot e responde o motivo do reset |
| `src/core/nmea.{h,cpp}`, `src/hal/gps_neo6m.cpp` | Altitude MSL da GGA, para o fallback |
| `src/main.cpp` | Restaura no boot, e persiste via mailbox flight → io |
| `test/test_phase/` | As dez invariantes da máquina de fases |

**Seis decisões que divergem do texto da issue ou o completam**

*A aceleração entra por um campo novo de `SensorSample`, e vale zero até a issue
07.* A máquina de fases consome o módulo |a|; o driver da IMU que o preenche é da
07. No alvo, sem aceleração, não há liftoff e a máquina fica na rampa — inerte até
a 07, mas toda a lógica de fase, referência e persistência já está no lugar e
testada com aceleração sintética.

*A persistência em NVS não roda na task de voo.* Uma escrita em NVS bloqueia alguns
milissegundos, e a task flight roda a 100 Hz — bloquear ali reabriria exatamente a
lacuna de amostras que a issue 05 fechou. A task flight monta o snapshot e ergue
uma flag; a task io (core 0) é quem grava. São ~5 eventos por voo, então a flag
quase nunca está levantada.

*O fallback de referência absurda usa o bit 6 do byte de saúde.* O byte de flags do
pacote já está cheio (fase, fonte, qualidade), mas o de saúde tinha bits livres. O
bit 6 é diferente dos outros — não é saúde de módulo, e sim "a altitude é relativa
à referência barométrica (1) ou caiu para o GPS (0)". `PACKET_FORMAT.md` foi
atualizado; é aditivo, então um decodificador que ignore o bit continua válido.

*A altitude do GPS teve que ser puxada pela cadeia inteira.* O fallback pede
altitude do GPS, e a `GgaReading`/`GpsFix` não a carregavam. O campo 9 da GGA
(altitude MSL) passou a ser parseado no `core/nmea` e levado até o `GpsFix`. É a
única parte da issue que toca o código do GPS da issue 02.

*A referência do cabeçalho do log é a reusada, quando há reuso; senão a primeira
leitura.* O cabeçalho é escrito uma vez no boot, antes de qualquer liftoff. Quando
o reset faz a referência salva ser reusada, ela vai para o cabeçalho; num boot
limpo, vai a primeira leitura do barômetro, como antes. A referência congelada no
liftoff é a que vale para a altitude ao vivo.

*Um teste pré-existente mudou de número, não de intenção.* `test_every_emitted_packet_carries_altitude`
subia "no chão" sem liftoff; com a referência agora sendo média lenta em vez de
tiro único, ela escorrega atrás da pressão e a altitude final caiu de ~85 para
~77 m. O que o teste protege — altitude em todo pacote, crescente na subida —
continua igual.

**O que as fatias seguintes herdam**

*A issue 07 preenche `accel_mg` com a IMU real*, e a partir daí o liftoff dispara no
alvo. O campo já existe em `SensorSample` e a máquina de fases já o consome.

*A issue 08 preenche a fonte de posição e a posição fundida*; a fase já viaja no
pacote ao lado delas.

*A issue 13 (pós-pouso) herda a transição de mão única para POUSADO* como o gatilho
do beacon de recuperação e da média filtrada de posição.
