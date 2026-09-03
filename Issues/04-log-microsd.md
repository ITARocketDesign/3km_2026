# 04 — Log durável no microSD e recuperação pós-voo

**Tipo:** AFK
**User stories:** 20, 21, 22, 23, 24, 25, 26, 27, 28, 82, 92, 93, 94, 103

## What to build

O registro durável do voo. A placa passa a gravar cada ciclo no cartão num formato que sobrevive a uma queda de energia em pleno voo, e uma ferramenta no host recupera todo registro válido varrendo o arquivo bruto.

**Registro de tamanho fixo, 64 B.** O tamanho não é arbitrário: 64 B por ciclo a 100 Hz dá 6,4 kB/s e **8 registros por bloco de 512 B**. Cada registro carrega marcador mágico, número de sequência, **contador de boot** e CRC.

**O CRC permanece no cartão** — ao contrário do pacote de rádio, aqui não existe camada de enlace que o forneça.

**O contador de boot vai em cada registro, não só no cabeçalho.** O arquivo pré-alocado pode cair nos clusters do voo anterior, e como o recuperador varre por magic + CRC, registros antigos validam perfeitamente e entram no meio do voo atual. Este é o campo que impede contaminação entre voos.

**Arquivo pré-alocado e contíguo; a tabela de alocação não é tocada durante o voo.** Metadados escritos exatamente duas vezes — criação e fechamento. Isso remove o modo de falha que destrói o arquivo inteiro, em vez de tentar agendar em volta dele. A biblioteca padrão do Arduino **não serve**: ela não suporta pré-alocação nem arquivo contíguo, que são o mecanismo inteiro.

Arquivo novo por boot, com índice incremental. Bloco de cabeçalho no offset 0: magic, versão do formato, contador de boot, pressão de referência de solo, revisão do mapa de pinos. Escritas alinhadas em blocos de 512 B. **Timestamps tirados na aquisição, nunca na escrita.**

**Regra dura de exclusão que entra no escalonador nesta fatia:** nenhuma escrita no cartão começa enquanto o PA de 1 W do E22 estiver ligado. A regra protege o **rail de 3,3 V**, não o barramento — por isso o **airtime do SX1276 não bloqueia escrita**, já que ele vive no rail de 5 V. E uma solicitação de transmissão **nunca interrompe** uma escrita em andamento.

**Uma falha de log nunca pode calar o link de recuperação.** Se o cartão falhar ou estiver ausente, a transmissão continua.

Persistência NVS entra aqui no mínimo necessário: contador de boot. Fase e referência barométrica entram na issue 06; timestamp de reset entra na issue 11.

> **Requisito escrito, para ninguém "otimizar" depois:** o registro de log deve ser suficiente para reconstruir a amostra de sensores que o produziu. Nenhum campo bruto pode ser removido sob o argumento de que já está na saída fundida. É isso que torna possível o harness de replay da issue 14.

## Acceptance criteria

- [x] `core/` com codec de log: serialização e desserialização do registro de 64 B com magic, sequência, contador de boot e CRC
- [x] O registro carrega os valores **brutos** (accel, gyro, pressão, temperatura, GPS) **e** os fundidos (posição, altitude, velocidade vertical, fonte de posição, fase, saúde) — *accel/gyro/fase/fundidos têm lugar reservado e valem zero até as issues 06–08*
- [x] Pressão bruta gravada ao lado da altitude derivada
- [x] `core/` com função de varredura/recuperação, testada no host
- [x] `hal/` com adaptador microSD usando biblioteca com suporte a pré-alocação e arquivo contíguo (SdFat), com timeout duro — *ver ressalva sobre o que "timeout duro" pode significar numa chamada síncrona*
- [x] `hal/` com adaptador de persistência não-volátil (NVS), expondo o contador de boot
- [x] Arquivo novo por boot, com índice incremental
- [x] Arquivo pré-alocado e contíguo; metadados escritos exatamente duas vezes
- [x] Bloco de cabeçalho no offset 0 com magic, versão, contador de boot, pressão de referência e revisão do mapa de pinos
- [x] Escritas alinhadas em blocos de 512 B
- [x] Timestamps tirados no instante da aquisição
- [x] Escalonador: nenhuma escrita emitida enquanto o PA do E22 está ligado
- [x] Escalonador: o airtime do SX1276 **não** bloqueia escrita
- [x] Escalonador: uma solicitação de transmissão espera uma escrita em andamento, nunca a interrompe
- [x] Teste nativo: um fluxo de bytes truncado num offset arbitrário ainda entrega todo registro completo quando varrido
- [x] Teste nativo: registros com CRC corrompido são rejeitados, não aceitos silenciosamente
- [x] Teste nativo: **registros com contador de boot diferente do cabeçalho são descartados**
- [x] Teste nativo (escalonador com rádio e cartão falsos): nenhuma escrita enquanto o PA de 1 W está ligado; airtime de 915 MHz não bloqueia; TX nunca interrompe escrita
- [x] Teste nativo: com o cartão marcado como ausente, a telemetria continua sendo emitida
- [ ] No target: um voo simulado de bancada grava um arquivo, e a ferramenta de recuperação reconstrói os registros a partir do arquivo bruto — *a metade de host já foi exercitada, ver abaixo*

## Blocked by

- Issue 03 (E22 e escalonador — a regra de exclusão precisa do estado de TX do E22)

## Estado da implementação

Tudo implementado menos o critério de target, que precisa de placa e cartão:
`pio test -e native` passa com 39 casos em quatro suítes, `pio run -e
heltec_wifi_lora_32_V2` compila, e os cinco greps de `DISCIPLINE.md` saem vazios.

A metade de host do último critério **já foi exercitada**: um arquivo sintético
com cabeçalho do voo 7, 40 registros, 5 registros residuais do voo 6 e a cauda
cortada em 23 bytes passa por `tools/recover_log` e sai com os 40 registros
certos, os 5 do voo anterior descartados e a cauda ignorada. O que falta é a
outra metade — que quem gravou o arquivo tenha sido a placa.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/log_codec.{h,cpp}` | Registro de 64 B, bloco de cabeçalho, CRC-16 e a varredura de recuperação |
| `src/hal/sd_log.{h,cpp}` | SdFat: arquivo pré-alocado e contíguo, duplo buffer de 512 B, desligamento por travada |
| `src/hal/boot_counter.{h,cpp}` | Contador de boot em NVS |
| `tools/recover_log.cpp`, `tools/Makefile` | Ferramenta de host: arquivo bruto → CSV |
| `test/test_log/` | As seis invariantes do formato |

**Seis decisões que divergem do texto da issue ou o completam**

*O layout de 64 B reserva desde já os campos das issues 06, 07 e 08.* Accel,
giro, fase, fonte de posição, posição fundida, velocidade vertical e as marcas
d'água de stack existem no formato e valem zero até a issue produtora chegar. O
motivo é que o formato vai para o cartão e para a ferramenta de recuperação:
acrescentar campo depois obrigaria a versionar o formato e a carregar um
decodificador por versão no harness de replay da issue 14 — que precisa ler o log
do PRIMEIRO voo com o decodificador de hoje.

*Posição bruta e posição fundida são campos separados, os dois no registro.* Não
é redundância: a fundida pode vir da ponte inercial ou da última válida com
idade, e sem a bruta ao lado a decisão da issue 08 vira irreversível depois do
voo. É a leitura literal do requisito escrito no texto da issue.

*Temperatura foi para i16 em centésimos de grau.* Cobre -327 a +327 °C com
resolução de 0,01 e devolveu 2 B ao orçamento — que foram para a posição fundida.
Pressão e altitude continuam em float, porque são as duas grandezas que o replay
compara numericamente.

*"Timeout duro" virou recusa de chamar de novo.* Uma escrita de SdFat é síncrona
e não pode ser interrompida no meio; não existe timeout que a preempte. O que
existe: medir a duração de cada bloco, contar como travada o que passar de 500 ms
(H13 fala em 100 ms típicos e 500 ms de pior caso), e **desligar o cartão** após
três travadas seguidas. Um cartão doente para de roubar do link de recuperação o
tempo que o link precisa. O critério está marcado, mas com esta leitura.

*O duplo buffer de 512 B não é otimização, é o que torna a regra de exclusão
viável.* Com escrita registro a registro, os 140 ms por segundo em que o PA do
E22 está no ar descartariam ~14 % do log. Enfileirar nunca toca o cartão — é
seguro no meio do airtime —, e só a escrita do bloco espera a janela. A 25 Hz um
bloco enche em 320 ms e os dois dão 640 ms de folga contra 140 ms de janela
fechada. O buffer circular de verdade, com descarte do registro mais antigo e as
duas tasks, continua sendo da issue 05.

*A interface do escalonador mudou de forma.* `update()` passou a receber
`TxSchedulerInput` e devolver `TxSchedulerDecision`, porque a arbitragem agora
atravessa nos dois sentidos: entra "há escrita em andamento", sai "pode começar a
escrever". A decisão é internamente consistente — se uma transmissão do E22 foi
liberada nela, `may_start_write` já vem falso —, então quem a executa pode
transmitir e escrever na ordem que quiser.

**As duas regras de exclusão não são simétricas, e isso é o ponto**

Elas protegem recursos diferentes, e tratá-las como uma só regra estraga uma das
duas:

| Regra | Recurso | Vale para |
|---|---|---|
| PA no ar ⇒ nenhuma escrita começa | **rail** de 3,3 V, ~1 A | só o E22 |
| Escrita em andamento ⇒ nenhuma TX começa | **barramento** SPI | os dois rádios |

O airtime do SX1276 não bloqueia escrita porque ele vive no regulador de 5 V do
Heltec; unificar as regras jogaria fora 8 % de janela de escrita por nada. Na
direção contrária, a escrita bloqueia os dois rádios porque quem tem o barramento
na mão é ela, e carregar a carga útil de qualquer rádio precisa dele.

**O que as fatias seguintes herdam**

*`write_in_progress` ainda é sempre falso no voo.* Neste laço único a escrita é
síncrona: ela começa e termina dentro de uma volta, então nunca há uma em
andamento quando o escalonador é consultado. A regra existe, está testada
nativamente, e passa a valer de verdade na issue 05, quando a escrita for de
outra task. O efeito prático hoje é indireto e correto: a escrita gasta tempo do
laço, o `millis()` anda, e a transmissão devida durante ela sai atrasada.

*A issue 05 substitui o duplo buffer.* `SdLog::append()` e `SdLog::service()` já
têm a forma certa — enfileirar não toca o cartão, escrever espera a janela —, mas
o descarte hoje é do registro MAIS NOVO, não do mais antigo. A política correta
(descartar o mais antigo, nunca uma leitura de sensor nem um pacote) vem com o
buffer circular.

*A issue 06 preenche a referência barométrica do cabeçalho.* Hoje é a primeira
leitura válida do barômetro, tirada no `setup()`. A média lenta na rampa, o
congelamento no liftoff e a persistência em NVS são de lá — e o campo do
cabeçalho já existe esperando.

*A issue 11 preenche as duas marcas d'água de stack*, e a 07 o contador de resets
do estimador. Os três campos já estão no layout.

*A issue 14 tem seu insumo pronto.* `tools/recover_log` entrega CSV com os brutos
e os fundidos lado a lado, que é exatamente o que o harness de replay precisa para
rodar o núcleo sobre um voo real e comparar as saídas.
