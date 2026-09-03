# 08 — Máquina de fonte de posição: GPS ↔ INS ↔ última válida

**Tipo:** AFK
**User stories:** 12, 41, 42, 43, 66, 79

## What to build

A máquina que decide de onde vem a posição transmitida, e que consequentemente decide entre pacote completo e pacote só-altitude.

Estados e transições:

- **GPS válido** → fonte GPS. O passo de correção zera continuamente a deriva inercial.
- **GPS perdido além de um limiar** → fonte INS: propaga a posição por integração inercial durante uma **janela curta e configurável**, para continuar transmitindo *uma* posição durante o vão.
- **Além da janela de ponte** → fonte "última válida": para de transmitir posição inercial e passa a transmitir **a última posição GPS válida mais a idade dela**.
- **GPS reaquirido** → volta a GPS e a estimativa reconverge.

**Por que a janela é curta e por que a última posição válida vence o INS depois dela.** Com uma IMU de 6 eixos sem magnetômetro, a posição horizontal dead-reckoned diverge em segundos a dezenas de segundos. O fallback inercial é uma **ponte** para vãos de 10–20 s, não navegação. Passada a janela útil, um fix antigo com a idade declarada vale mais — para achar o foguete — que uma posição inercial derivada, e é mais honesto com a equipe de solo.

**A flag de confiança emitida por esta máquina decide a forma do pacote:** completo quando a posição é confiável, só-altitude quando não é. **Altitude está presente nas duas formas** — a equipe nunca tem silêncio total de telemetria.

A flag de fonte vai em todo pacote, para a estação de solo saber quanto confiar na posição. Isso é requisito, não conveniência: a equipe precisa confiar mais nos fixes de origem GPS do que nos de origem inercial.

**Se a IMU estiver ausente ou morta, o fallback inercial é desabilitado** e o sistema continua em GPS + barômetro.

A média filtrada de fixes pós-pouso é uma extensão desta máquina e entra na issue 13.

## Acceptance criteria

- [ ] `core/` com máquina de fonte de posição, pura, tempo como parâmetro
- [ ] Janela de ponte inercial configurável
- [ ] Estado "última válida" carrega a idade do fix
- [ ] Flag de fonte de posição codificada nos 2 bits do byte de flags do pacote
- [ ] Flag de confiança decide pacote completo vs. só-altitude
- [ ] IMU ausente ou morta desabilita o fallback inercial
- [ ] Teste nativo: GPS válido → fonte GPS
- [ ] Teste nativo: N amostras obsoletas → fonte INS
- [ ] Teste nativo: além da janela → fonte "última válida" **com a idade correta**
- [ ] Teste nativo: GPS retorna → fonte GPS e reconvergência
- [ ] Teste nativo: passada a janela, o pacote carrega última posição válida com idade, e **não** posição inercial derivada
- [ ] Teste nativo: com a IMU marcada como ausente, nenhum fallback inercial é tentado e a telemetria de GPS + barômetro continua fluindo
- [ ] Teste nativo: altitude aparece em **todo** pacote emitido, nas duas formas

## Blocked by

- Issue 02 (GPS e pacote completo)
- Issue 07 (estimador — a propagação inercial é o passo de predição)

## Estado da implementação

Tudo implementado menos o critério de target implícito: um vão real de GPS em
campo produzindo a sequência Gps → Ins → LastValid, que precisa de placa + GPS.
`pio test -e native` passa com 73 casos (eram 64): 7 novos na suíte
`test_position_source` e 2 na `test_core`. `pio run -e heltec_wifi_lora_32_V2`
compila (RAM 26,1 %), e os cinco greps de `DISCIPLINE.md` saem vazios.

A máquina vive inteira no `core/`, pura e com o tempo como parâmetro. Ela decide a
fonte pela IDADE do último fix válido, não pelo ciclo — é essa a diferença para a
distinção grosseira Gps/Ins que o estimador expõe, que pisca a cada amostra (a
25 Hz de barômetro contra 5 Hz de GPS, quatro em cinco ciclos não têm correção).

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/position_source.{h,cpp}` | A máquina de quatro estados, pura; entrada, saída e config |
| `src/core/flight_computer.{h,cpp}` | Passou a compor a máquina; ela decide forma do pacote, fonte e posição, e a fonte vai ao log |
| `test/test_position_source/` | As sete invariantes da máquina |
| `test/test_core/` | Dois testes de seam: LastValid no ar depois da janela, e a fonte no log |

**Três decisões que divergem do texto da issue ou o completam**

*A idade do fix NÃO vai no ar.* O pacote de 20 B não tem campo de idade e
acrescentá-lo furaria o orçamento de airtime (`PACKET_FORMAT.md`). No ar, a fonte
LastValid nos 2 bits do byte de flags é o sinal de que a posição é velha; a idade
numérica sai na saída da máquina e a fonte vai ao registro de log — e como o
formato de 64 B já grava fonte + t_ms por registro, a idade se reconstrói no
pós-voo sem campo novo. O formato do cartão continua congelado.

*IMU ausente cai para LastValid, não para só-altitude.* Sem IMU a ponte Ins é
desabilitada, mas a última posição válida com idade não depende de IMU e ainda
ajuda a achar o foguete — é mais honesta que silêncio de posição. "Ausente" é a
IMU que nunca produziu amostra válida (latch em `imu_valid`); a morte a meio-voo é
degradação da issue 10.

*A reconciliação de referencial da altitude do GPS ficou de fora.* As notas da 07
apontaram esta máquina como o lugar dela, mas os critérios de aceitação da 08 não a
pedem — eles são sobre fonte de posição e forma do pacote. Fica para quem precisar
dela (o fallback de barômetro ausente já sinaliza a troca de referencial pelo bit
kAltRef da 06).

**O que as fatias seguintes herdam**

*A issue 13 (pós-pouso)* herda a máquina de fonte pronta; a média filtrada de fixes
pós-pouso é uma extensão dela, como a própria issue 08 antecipa.
