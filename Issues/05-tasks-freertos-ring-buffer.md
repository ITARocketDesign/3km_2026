# 05 — Duas tasks FreeRTOS e ring buffer SPSC lock-free

**Tipo:** AFK
**User stories:** 89, 90, 91, 95, 96, 97, 108

## What to build

A separação de concorrência que impede o cartão de engolir amostras de sensor.

**O problema concreto:** um microSD para de responder por 100–250 ms durante coleta de lixo interna — comportamento normal, sem erro. Num superloop isso engole as amostras da IMU, e o passo de predição do filtro integra aceleração com um Δt errado, **sistematicamente, durante o boost**. Uma lacuna nas amostras não é ruído, é viés. Com tasks separadas a travada fica no core 0 e o core 1 não percebe.

| Task | Core | Prioridade | Stack | Responsabilidade | Barramentos |
|---|---|---|---|---|---|
| `flight` | 1 | alta | **8 KB** | Poll da IMU e do barômetro, drenagem da UART do GPS, estimador, decisões | **só I²C e UART** |
| `io` | 0 | média | **12 KB** | Dono exclusivo do SPI: cartão + os dois rádios, arbitragem de chip-select | **só SPI** |

O stack sizing é conservador por escolha: a task `io` roda SdFat (2–4 KB por operação de mount/pre-allocate/write) e RadioLib; a task `flight` roda o Kalman com matrizes 4×4 em float. Com 32 KB de ring buffer e 20 KB de stack total, sobram ~248 KB dos ~300 KB de SRAM do ESP32.

**Ring buffer lock-free SPSC, no `core/`.** A task `flight` é o único produtor; a task `io` é o único consumidor. Isso é estrutural, não convencional — decorre da separação de barramentos que fundamenta todo o design de concorrência. Se alguém precisar de um segundo consumidor, a arquitetura mudou e o buffer precisa ser reprojetado, não adaptado.

- Índices de escrita e leitura em `std::atomic`, com `memory_order_acquire`/`memory_order_release`. **Sem mutex, sem semáforo, sem primitiva FreeRTOS** — qualquer uma delas introduz inversão de prioridade ou latência no loop de aquisição a 100 Hz.
- `std::atomic` é C++11 padrão e funciona tanto no Xtensa quanto em x86/ARM, que é o que permite testar a sincronização no host.
- **Descarte do mais antigo:** quando o produtor detecta buffer cheio, avança o índice de leitura atomicamente. O consumidor vê um índice que pulou, e a contagem de descartados é derivável pela diferença de sequência. **Nunca atrasar uma leitura de sensor nem um pacote de telemetria.**
- 512 slots de 64 B = 32 KB, cobrindo a pior janela de rádio (SF12 = 1712 ms) somada à pior travada de cartão (500 ms) com margem de ~5 s.

**Alocação estática:** ring buffer, buffers de bloco de 512 B e buffers de UART alocados estaticamente ou uma única vez no boot. Nenhuma alocação dinâmica durante o voo.

Os estados de arbitragem são eventos de duração variável, não fatias fixas de tempo — uma travada de cartão degrada a resolução do log em vez de quebrar o ciclo.

## Acceptance criteria

- [x] `core/` com ring buffer SPSC lock-free usando `std::atomic`, sem nenhuma primitiva FreeRTOS
- [x] 512 slots de 64 B (32 KB), alocado estaticamente
- [x] Política de descarte por avanço atômico do índice de leitura — *observado na leitura, não no push; ver ressalva abaixo sobre único-escritor-por-índice*
- [x] Duas tasks FreeRTOS fixadas em cores diferentes: `flight` no core 1, `io` no core 0
- [x] Stacks de 8 KB (`flight`) e 12 KB (`io`)
- [x] Task `flight` usa **apenas** I²C e UART — nenhuma chamada de SPI, verificável em revisão
- [x] Task `io` é a **única** que toca SPI
- [x] Task `io` executa as ações que o escalonador decidiu, sem política própria
- [x] Nenhuma alocação dinâmica depois do boot
- [x] Taxa de ciclo de 100 Hz (Δt = 10 ms); barômetro a 25 Hz, GPS a 5 Hz — *IMU a 100 Hz é da issue 07; o ciclo já roda a 100 Hz esperando por ela*
- [x] Teste nativo: produtor a 100 Hz e consumidor drenando em rajadas com pausas de até 500 ms — nenhum registro perdido enquanto o buffer não enche
- [x] Teste nativo: buffer cheio descarta os mais antigos, o produtor **nunca bloqueia**, e o consumidor vê a lacuna pela descontinuidade na sequência
- [x] Teste nativo: operação concorrente com threads no host — nenhum data race, nenhum registro corrompido
- [ ] No target: uma travada induzida de cartão de ~250 ms não produz lacuna nas amostras de sensor — *precisa de placa e cartão*

## Blocked by

- Issue 04 (log no microSD — o consumidor precisa ter o que consumir)

## Estado da implementação

Tudo implementado menos o critério de target, que precisa de placa e cartão:
`pio test -e native` passa com 45 casos em cinco suítes (as seis novas do ring),
`pio run -e heltec_wifi_lora_32_V2` compila (RAM em 25,8 %), e os cinco greps de
`DISCIPLINE.md` saem vazios. O ring foi exercitado também sob ThreadSanitizer —
produtor e consumidor em threads reais, 200 k registros, estouro forçado: nenhuma
corrida reportada, contabilidade fechada, nenhum registro rasgado.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/ring_buffer.h` | O SPSC lock-free, header-only, template em capacidade e slot |
| `test/test_ring/` | As seis invariantes do ring, incluindo a concorrente com threads |
| `src/main.cpp` | As duas tasks fixadas, os dois rings e os dois sinais de arbitragem |
| `src/hal/sd_log.{h,cpp}` | Reescrito para montagem de bloco único drenado do ring |

**Cinco decisões que divergem do texto da issue ou o completam**

*O descarte é observado na leitura, não avançado no push.* A issue diz "o produtor
avança o índice de leitura no cheio". Ao pé da letra, os DOIS lados escreveriam o
índice de leitura — que é, ela mesma, a corrida de dados que o critério concorrente
proíbe. Cada índice tem um único escritor: o produtor só escreve `write_index_`, o
consumidor só escreve `read_index_`. O comportamento observável é idêntico — o mais
antigo é descartado, o produtor nunca bloqueia, o consumidor vê a lacuna e o
`dropped()` é derivável —, mas o descarte é contabilizado quando o consumidor
percebe que ficou mais de uma volta para trás e salta para o mais antigo ainda
válido.

*Um slot fica de guarda: capacidade útil 511, não 512.* O slot em voo do produtor é
sempre `write_index_ % 512`; se o consumidor lesse até encher os 512, o mais antigo
dele coincidiria com esse slot e sairia rasgado. Reservar um slot mantém o mais
antigo sempre atrás do slot em voo. A 6,4 kB/s isso ainda são ~5,1 s de fôlego,
muito além da pior janela combinada de rádio e cartão. Um `memcpy` de 64 B não é
atômico, então o consumidor faz um seqlock: relê `write_index_` depois da cópia e,
se o produtor deu uma volta inteira desde então, descarta a cópia e recomeça.

*O ring é um template, instanciado duas vezes.* A separação de barramentos cria
DOIS caminhos flight → io: o log e os comandos de transmissão. Só a task io toca
SPI, então a task flight não pode transmitir — ela decide e enfileira. Um segundo
anel de 32 KB estouraria o orçamento de SRAM, então o tx-ring é pequeno
(`RingBufferT<16, 24>` = 384 B). O descarte do mais antigo serve os dois: se a task
io travasse a ponto de encher o tx-ring, mandar a posição mais nova é melhor que
recusá-la. A telemetria nunca é atrasada no caminho nominal — o escalonador solta
~2 transmissões por segundo e a task io as drena a cada volta.

*`SdLog` perdeu o duplo buffer e o descarte.* A elasticidade e o descarte do mais
antigo mudaram para o ring, como a issue 04 previu. `SdLog` agora só monta um bloco
de 512 B com registros JÁ codificados que a task io tira do ring, e o grava quando
o bloco enche e o escalonador libera. A task io para de drenar o ring enquanto o
bloco pronto não foi gravado — o backlog fica no ring, que é quem sabe descartar.

*`write_in_progress` passou a valer de verdade.* Na issue 04 era sempre falso, num
laço único onde a escrita começava e terminava na mesma volta. Agora a task io o
levanta em volta da escrita real e o escalonador na task flight o lê: uma escrita
em andamento barra qualquer transmissão (disputa de barramento), e nunca é
interrompida. A regra simétrica de rail continua no escalonador: `may_start_write`
sai da task flight para a task io e barra a escrita enquanto o PA do E22 está no ar.

**O que as fatias seguintes herdam**

*O canal inercial a 100 Hz é da issue 07.* O ciclo já roda a 100 Hz (Δt = 10 ms), a
cadência de projeto; o barômetro é lido a 25 Hz dentro dele. Nas voltas sem leitura
fresca de barômetro o registro sai com `baro_valid` falso e o campo inercial em
zero — é a issue 07 que preenche a IMU a 100 Hz e propaga a altitude entre leituras
do barômetro.

*As marcas d'água de stack das duas tasks são da issue 11.* Os handles das tasks já
existem; ler `uxTaskGetStackHighWaterMark` de cada uma e gravar nos dois campos do
registro é de lá. Os campos já estão no layout de 64 B desde a issue 04.
