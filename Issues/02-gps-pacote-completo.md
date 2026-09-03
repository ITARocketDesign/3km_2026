# 02 — Posição GPS e pacote completo de 20 B

**Tipo:** AFK
**User stories:** 12, 13, 15, 19, 35, 36, 37, 38, 45, 66

## What to build

O pacote passa a carregar posição. Estende a fatia 01 com o NEO-6M, a configuração UBX que impede o receptor de rejeitar a própria solução no boost, e o pacote completo de 20 B.

**Por que a configuração é o núcleo desta issue, e não um detalhe:** o NEO-6M sai de fábrica no modelo dinâmico *Portable*, que assume menos de 1 g. Sob os 10–20 g do boost o receptor rejeita a própria solução e derruba o fix, e a reaquisição leva 10–60 s — a equipe perde a subida inteira. Pior: uma queda de rail devolve o receptor para *Portable* silenciosamente, então configurar uma vez no boot não basta.

Configuração enviada no boot e **reaplicada ao detectar reset do receptor**:

- Modelo dinâmico **Airborne <4g**
- Sentenças NMEA não usadas desligadas; taxa elevada para 5 Hz
- Configuração salva na memória com bateria de backup, para sobreviver a um reset por queda de rail

Detecção de reset: fluxo NMEA reiniciando, ou contagem de satélites caindo a zero logo após uma transmissão.

**Buffer UART de 512 bytes** configurado antes do `begin()`. O default do ESP32 é 256 B, e o GPS a 5 Hz gera ~800 B/s de NMEA; uma ocupação temporária da task de aquisição transborda o buffer e perde sentenças em silêncio. A 9600 baud, 512 B dão ~530 ms de folga. O flag de overflow é verificado periodicamente e alimenta um contador — **sem ação corretiva**, a sentença perdida já foi perdida; o contador é diagnóstico para a próxima revisão.

**Pacote completo — 20 B**, little-endian:

| Off | Tipo | Campo |
|---|---|---|
| 0 | `u8` | magic (4 bits) + versão (4 bits) |
| 1 | `u16` | sequência — global, compartilhada pelos dois rádios |
| 3 | `u16` | tempo, em decissegundos desde o liftoff |
| 5 | `i32` | latitude × 1e7 |
| 9 | `i32` | longitude × 1e7 |
| 13 | `i16` | altitude, metros acima da referência |
| 15 | `i16` | velocidade vertical, dm/s |
| 17 | `u8` | flags: fase (3 b) + fonte de posição (2 b) + qualidade de fix (3 b) |
| 18 | `u8` | saúde: bitmap {imu, baro, gps, sd, e22, sx1276} |
| 19 | `u8` | GPS: satélites (4 b) + HDOP (4 b) |

**Os 20 B não são negociáveis.** Airtime LoRa é quantizada em blocos de 8 símbolos, e o teto do bucket-alvo é 21 bytes. Um pacote de 22 B pula um degrau e fura o teto de ciclo de trabalho já dimensionado nos docs de hardware. Não há CRC de aplicação no pacote de rádio — o LoRa já tem CRC de hardware e o driver não entrega pacote reprovado.

Nesta fatia a decisão entre pacote completo e só-altitude é simples: fix válido → completo, sem fix → só-altitude. A máquina de fonte de posição, com janela de ponte inercial e última posição válida, entra na issue 08.

O limite CoCom (velocidade **e** altitude simultaneamente altas) não se aplica nesta trajetória — a velocidade fica bem abaixo do limiar.

## Acceptance criteria

- [x] `hal/` com adaptador NEO-6M: UART, parsing NMEA, configuração UBX — *o parsing acabou em `core/`, ver Estado da implementação*
- [x] `Serial1.setRxBufferSize(512)` chamado **antes** de `Serial1.begin()`
- [x] Boot envia UBX-CFG-NAV5 com modelo dinâmico Airborne <4g
- [x] Boot desliga as sentenças NMEA não usadas e eleva a taxa para 5 Hz
- [x] Configuração salva na memória com bateria de backup (UBX-CFG-CFG)
- [x] Reset do receptor é detectado por fluxo NMEA reiniciando ou satélites caindo a zero após uma transmissão
- [x] Ao detectar reset, a configuração **inteira** é reaplicada
- [x] Flag de overflow da UART verificado periodicamente, alimentando um contador exposto ao log
- [x] `core/` com codec do pacote completo, layout exatamente como a tabela acima
- [x] Teste nativo: ida e volta do pacote completo
- [x] Teste nativo: o tamanho codificado do pacote completo é **exatamente 20 B**
- [x] Teste nativo: altitude decodifica corretamente das **duas** formas de pacote
- [x] Teste nativo: fix válido produz pacote completo; ausência de fix produz só-altitude
- [x] Teste nativo: número de sequência incrementa e é global
- [x] Documentação do formato para a estação de solo, incluindo a saturação de `t` em 65535 ds ≈ 109 min e a instrução de usar o número de sequência para ordenar depois disso
- [ ] No target: pacote completo com lat/lon coerentes recebido no chão

## Blocked by

- Issue 01 (tracer bullet — scaffold, codec, FlightComputer, SX1276)

## Estado da implementação

Tudo implementado menos o último critério, que precisa da placa: `pio test -e
native` passa com 22 casos em duas suítes, e `pio run -e heltec_wifi_lora_32_V2`
compila.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/nmea.{h,cpp}` | Analisador NMEA: enquadramento, checksum, GGA → campos do pacote |
| `src/core/telemetry_codec.{h,cpp}` | As duas formas do pacote, num par `encode_packet`/`decode_packet` |
| `src/hal/gps_neo6m.{h,cpp}` | UART, configuração UBX, detecção de reset, contador de overflow |
| `test/test_nmea/` | Suíte do analisador, contra sentenças de checksum real |
| `ELE3km/PACKET_FORMAT.md` | O formato para quem escreve o decodificador da estação de solo |

**Quatro decisões que divergem do texto da issue ou o completam**

*O analisador NMEA ficou em `core/`, não em `hal/`.* O critério dizia `hal/`, mas
enquadramento, checksum e a conversão de "ddmm.mmmmm" são a parte do caminho do
GPS que erra em silêncio — um erro em qualquer um dos três vira uma posição
plausível e errada. Em `core/` ela roda na suíte nativa contra sentenças de valor
conhecido, incluindo o exemplo canônico da norma como âncora externa. A HAL ficou
com o que depende de hardware e de tempo.

*Não se espera ACK do receptor, nem no boot nem na reaplicação.* A detecção de
reset já é a verificação: se a configuração não pegou, as sentenças que mandamos
desligar continuam chegando e a configuração é reenviada. Esperar ACK
acrescentaria uma espera bloqueante no meio do voo para chegar à mesma conclusão
mais devagar. Há um piso de 2 s entre reconfigurações para as sentenças já em
trânsito não dispararem o detector em cascata.

*O limiar de overflow é 480 B, não 512.* O buffer circular do driver não é
garantidamente preenchível até o último byte, e um limiar em 512 exato poderia
nunca disparar — um contador de diagnóstico que nunca dispara é pior que nenhum.
Ele erra para o lado de reportar demais, que é o lado certo aqui.

*O codec da issue 01 foi reescrito.* `encode_altitude_packet` /
`decode_altitude_packet` viraram `encode_packet` / `decode_packet`, que despacham
pela forma — na codificação por `packet.form`, na decodificação pelo comprimento.
As duas formas diferem por um único bloco de 8 B no meio do layout, e duplicar o
resto é exatamente onde elas divergiriam em silêncio.

**O que as fatias seguintes herdam**

*A issue 03 precisa mudar a semântica do número de sequência.* Hoje
`FlightComputer::update()` incrementa `packet_sequence_` **por pacote emitido**,
o que com um rádio só é indistinguível de "por ciclo". A issue 03 exige o **mesmo
número nos dois rádios** defasados em 500 ms, então o contador passa a andar por
ciclo de telemetria e o escalonador guarda o pacote até a vez de cada rádio.
`test_sequence_increments_across_packet_forms` fixa o comportamento atual e vai
precisar ser revisto junto.

*O E22 também precisa chamar `note_transmission()`.* Hoje só o caminho do SX1276
chama, e o detector de brown-out do receptor existe justamente por causa do PA de
1 W do E22 (hazard H3) — 100 mW do SX1276 é o caso menos provável dos dois.

*`kMaxPacketsPerUpdate` já é 2* e `Radio::E22` já existe em `core/types.h`;
`board_early_init()` mantém `PIN_E22_NRST` em LOW e o comentário lá marca que o
pino passa para a biblioteca de rádio na issue 03.

*`platformio.ini` ganhou `test_ignore = test_*`* — uma suíte nativa nova não
precisa mais ser registrada num segundo lugar.
