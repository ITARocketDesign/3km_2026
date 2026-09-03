# 03 — Segundo rádio: E22 fail-active e escalonador de arbitragem

**Tipo:** AFK
**User stories:** 3, 14, 18, 61, 62, 63, 64, 65, 67, 68, 69, 70

## What to build

O segundo caminho de rádio entra no ar, e com ele nasce o escalonador de arbitragem no `core/`.

**Fail-active, não fail-over.** Os dois rádios transmitem desde o boot, em cadências espelhadas, **sem nenhuma lógica de detecção acoplando um ao outro**. Isso é deliberado e é a decisão mais fácil de "corrigir" errado numa revisão futura: as falhas do E22 que importam são invisíveis ao firmware — RadioLib retorna sucesso com a antena solta, com o PA queimado, ou com o rail afundando a cada burst de 1 A. Um failover disparado por detecção não dispararia. Rodando os dois em paralelo, a equipe descobre a falha pelos dados, no chão, e ganha diversidade de recepção porque os dois links falham por motivos independentes.

| | E22 / 433 MHz | SX1276 / 915 MHz |
|---|---|---|
| Potência | +30 dBm | +20 dBm |
| Regulador | buck de 3,3 V (compartilhado com sensores e SD) | regulador interno do Heltec (buck de 5 V) |
| Recurso escasso | **airtime e rail** | praticamente nenhum |
| Cadência em voo | 1 Hz | 1 Hz, defasado ~500 ms |

**Escalonador de arbitragem, no `core/` puro.** Recebe estado + tempo, devolve a próxima ação. Nenhuma dessas políticas pode viver na task — as tasks são bombas burras que executam o que o escalonador decidiu. É este posicionamento que torna as invariantes de recurso testáveis no host.

Políticas nesta fatia:

- **Defasagem de 500 ms entre os dois rádios, verificada como invariante, não presumida.** Duas justificativas independentes: não empilhar as duas transmissões na mesma janela da task de I/O, e não somar os dois consumos no nó de bateria compartilhado pelos dois bucks.
- **Teto de ciclo de trabalho verificado em código**, limitando as transmissões numa janela deslizante. A lógica de cadência não pode furar esse teto, seja qual for a fase.
- **Número de sequência global único**, compartilhado pelos dois rádios, para a estação de solo deduplicar e montar uma trajetória única a partir dos dois fluxos.
- **Três chip-selects no barramento SPI, exatamente um ativo por vez.** GPIO18 (SX1276) deixa de ser "HIGH para sempre" e vira um chip-select normal — mas o passo de boot que o coloca em HIGH antes de `SPI.begin()` continua obrigatório, porque o pino flutua no reset e o rádio se auto-seleciona.

A regra de exclusão rail-vs-escrita entra na issue 04, quando existir cartão para escrever. A degradação de cadência pós-pouso entra na issue 13.

**O recurso escasso é o rail, não o barramento:** a carga útil é escrita no rádio antes de a transmissão ser disparada, e durante o airtime o rádio transmite sozinho com o barramento livre.

## Acceptance criteria

- [x] `hal/` com adaptador E22-400M30S (SX1268, SPI, 433 MHz, +30 dBm)
- [x] Fator de espalhamento de voo escolhido de modo que o airtime caiba no ciclo de trabalho seguro para o PA e para o rail, com o cálculo registrado na issue — *SF8, cálculo abaixo*
- [x] `core/` com escalonador de arbitragem: recebe estado + tempo, devolve a próxima ação
- [x] Os dois rádios transmitem desde o boot, sem nenhuma lógica de detecção ligando um ao outro
- [x] Defasagem de ~500 ms entre as transmissões, **verificada pelo escalonador**
- [x] Teto de ciclo de trabalho em janela deslizante, aplicado pelo escalonador
- [x] Mesmo número de sequência nas transmissões dos dois rádios
- [x] Três chip-selects vivos, exatamente um ativo por vez; GPIO18 HIGH antes de `SPI.begin()` mantido
- [x] GPIO14 (reset do SX1276) controlado pelo driver, não mantido em LOW
- [x] Só a biblioteca de rádio toca os pinos do comutador de RF — nenhuma escrita direta
- [x] TX não-bloqueante com IRQ nos dois rádios; borda de IRQ sempre confirmada contra o registrador de status
- [x] Timeout limitado no sinal de ocupado, com caminho de recuperação por reset, nos dois rádios
- [x] Teste nativo (escalonador com rádios falsos): as duas transmissões nunca se sobrepõem
- [x] Teste nativo: o teto de ciclo de trabalho nunca é furado, mesmo quando a lógica de cadência pede mais
- [x] Teste nativo: a sequência emitida para os dois rádios é idêntica no mesmo ciclo
- [ ] No target, **com as duas antenas conectadas**: dois receptores, um por banda, recebem o mesmo número de sequência defasado em ~500 ms

## Blocked by

- Issue 02 (GPS e pacote completo)

## Estado da implementação

Tudo implementado menos o último critério, que precisa da placa, das duas
antenas e de dois receptores: `pio test -e native` passa com 29 casos em três
suítes, `pio run -e heltec_wifi_lora_32_V2` compila, e os cinco greps de
`DISCIPLINE.md` saem vazios.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/tx_scheduler.{h,cpp}` | O escalonador: época do ciclo, dois slots, defasagem, separação mínima, janela deslizante de ciclo de trabalho, número de sequência |
| `src/hal/radio_e22.{h,cpp}` | Adaptador do E22-400M30S: SX1268 em 433 MHz, SF8, +30 dBm, TX não-bloqueante |
| `test/test_scheduler/` | As seis invariantes de recurso, com o escalonador dirigido direto |
| `src/core/flight_computer.{h,cpp}` | Perdeu o contador de sequência e a lógica de cadência; ganhou o escalonador |

**O fator de espalhamento, e o cálculo que o critério pede**

SF8, e a escolha é mitigação de hazard e não preferência de alcance. Com
BW 125 kHz, CR 4/8, CRC ligado, cabeçalho explícito e preâmbulo de 8 símbolos
(tabela de §H1 dos hardware constraints):

| SF | Airtime de 20 B | Ciclo de trabalho a 1 Hz |
|---|---:|---:|
| 7 | 78 ms | 7,8 % |
| **8** | **140 ms** | **14 %** |
| 9 | 247 ms | 24,7 % |
| 11 | 987 ms | 98,7 % |

SF8 a 1 Hz gasta 14 % e deixa ~860 ms de silêncio de rádio por segundo, que é o
que a issue 04 vai precisar para escrever no cartão sem coincidir com o pico de
1 A. SF11 — o valor do rascunho original em `E22_integration.md` — deixaria o PA
ligado quase todo o tempo, sem nenhum "entre as rajadas" onde esconder uma
escrita, e é problema térmico e de bateria por si só (H1c). O enlace em voo não
justifica o gasto: ~103 dB de perda de espaço livre a 8 km em 433 MHz contra
+30 dBm e uma Yagi no solo deixam dezenas de dB de margem em SF8, mesmo com o
foguete girando. O SF12 aparece só no beacon pós-pouso, na issue 13.

O SX1276 fica em SF7, como na issue 01: 78 ms, 7,8 %, e ele nem está no rail
disputado.

**Seis decisões que divergem do texto da issue ou o completam**

*O escalonador ficou dono do número de sequência, não só do agendamento.* O
texto da issue trata a sequência como um dado do pacote; ela é artefato de
agendamento. Quem sabe que um ciclo de telemetria começou é o escalonador, e é
por ciclo que o contador anda. A divisão que saiu disso é limpa: o escalonador é
dono de **quando**, de **qual rádio** e da **sequência**; o `FlightComputer` é
dono do **conteúdo**, e monta um pacote candidato a cada amostra sem saber se
algum rádio vai transmiti-lo.

*O segundo rádio transmite a cópia congelada no começo do ciclo.* O que o SX1276
põe no ar tem 500 ms de idade. A alternativa — remontar com dados frescos e
reusar só o número — foi descartada porque `PACKET_FORMAT.md` já está congelado
com a estação de solo: dois pacotes com o mesmo número de sequência são
idênticos, e deduplicar um deles nunca descarta informação. Se o conteúdo
divergisse, qual dos dois sobrevive passaria a depender de qual receptor ouviu
primeiro.

*Cobra-se sempre o airtime do pacote COMPLETO.* A forma só-altitude é mais
barata, então cobrar 140 ms por qualquer transmissão do E22 gasta orçamento
demais — erro para o lado seguro — e evita uma tabela de airtime por forma
dentro do núcleo, que teria que ser mantida em sincronia com o codec.

*A defasagem é verificada por uma separação mínima de 200 ms entre transmissões,
não pela conferência do agendamento nominal.* No voo nominal ela nunca dispara,
porque o intervalo é de 500 ms; ela existe para o dia em que a lógica de cadência
estiver errada — uma fase futura com outra cadência, por exemplo. 200 ms cobre
com margem o maior airtime dos dois rádios (140 ms). Um slot barrado é **adiado**,
não descartado, e como os instantes nominais saem da época do ciclo e não da
última transmissão, o atraso não se propaga para o ciclo seguinte.

*O teto ficou em 20 % para os dois rádios, sobre uma janela de 10 s.* O ponto de
projeto é 14 % (E22) e 7,8 % (SX1276); os 20 % dão folga para a cadência errar um
pouco e ainda assim serem pegos. A janela é um anel de 32 entradas por rádio,
dimensionado para que o teto sempre morda antes do anel; anel cheio recusa a
transmissão, que é o lado seguro de errar.

*Uma parada do laço reancora a época em vez de recuperar os ciclos perdidos.*
Não estava no texto da issue e apareceu escrevendo o teste: atrasado mais de um
período, o escalonador começava um ciclo por volta do laço até alcançar o
presente, queimando números de sequência que quase nunca chegavam a transmitir —
a separação mínima barrava a maioria. A estação de solo veria um buraco na
sequência e mediria perda que nunca existiu, e o buraco na sequência é
justamente o sinal de perda dela. Os ciclos perdidos ficam perdidos; o tempo
decorrido continua visível no campo `t`. Isso importa para as issues 04 e 05, que
são exatamente de onde as paradas vão vir.

**Três testes da issue 02 foram revistos**

Eles fixavam a semântica de um rádio só, exatamente como o handoff da 02 avisou.
`test_sequence_increments_across_packet_forms` virou
`test_sequence_advances_per_cycle_across_packet_forms`. Os outros dois —
altitude em todo pacote, e sobrevivência à queda do barômetro — mudaram de
contagem e ganharam a distinção entre "dentro do ciclo" e "entre ciclos".

O de queda do barômetro ganhou uma exceção que vale registrar: o SX1276 ainda põe
no ar a cópia do último ciclo saudável meio segundo depois da falha, **com o bit
de saúde que aquele ciclo tinha**. Não é telemetria velha por acidente — é o
mesmo pacote, com o mesmo número de sequência, que o E22 já transmitiu. A partir
do ciclo seguinte o bit apaga.

**Um comentário reescrito para não quebrar um grep**

O aviso sobre GPIO25 em `radio_e22.h` é onde ele mais precisa estar, mas escrito
com todas as letras ele fazia o grep da regra 1 de `DISCIPLINE.md` acusar um
falso positivo permanente. O aviso ficou; a palavra que o grep procura foi
omitida de propósito, com nota explicando por quê. Um grep de verificação que
nunca sai vazio para de ser verificação.

**O que as fatias seguintes herdam**

*A issue 04 tem o silêncio de rádio que precisa, mas a regra de exclusão ainda
não existe.* O escalonador conhece airtime e sabe quando cada rádio dispara, que
é a informação de que a regra rail-vs-escrita precisa; falta a entrada de
"escrita em andamento" e a decisão correspondente. É extensão do
`TxScheduler::update`, não módulo novo.

*A issue 13 muda cadência e SF por fase, e os dois já são configuração.*
`TxSchedulerConfig` tem período e orçamento por rádio; o SF ainda é constante
dentro de `radio_e22.cpp` e precisa virar parâmetro de `begin()` ou um
`set_flight_profile()`.

*A issue 05 precisa serializar o barramento.* Hoje os três chip-selects
convivem porque o laço é único e a carga útil é escrita antes do disparo, com o
barramento livre durante o airtime. Com duas tasks isso vira mutex de SPI.

*Uma transmissão recusada pela HAL já foi cobrada no orçamento.* Se
`start_send()` devolve false — rádio ocupado, biblioteca recusando — o
escalonador já debitou o airtime. Conservador de propósito, mas se a issue 10
quiser contabilizar transmissões perdidas, o número certo não está no
escalonador.

*A tensão do TCXO do E22 é a única incógnita de hardware que sobrou.* Está em
1,8 V, o valor da série E22 da Ebyte, e não está registrada em nenhum documento
do projeto. Se `begin()` falhar no target com chip não encontrado ou timeout de
comando SPI, essa constante é a primeira coisa a mexer.
