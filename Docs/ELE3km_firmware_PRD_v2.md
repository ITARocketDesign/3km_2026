# PRD — Firmware do Computador de Bordo ELE3km (v2)

**Projeto:** ELE3km — computador de bordo com telemetria para foguete de competição
**Alvo:** Heltec WiFi LoRa 32 **V2** (ESP32) como computador de bordo
**Transmissores:** Ebyte **E22-400M30S** (SX1268, 433 MHz, 1 W) **e** o **SX1276 onboard** (915–928 MHz, 100 mW) — dois caminhos independentes
**Build:** PlatformIO — hardware atrás de uma HAL fina, lógica de voo num `core/` puro testável no host, testes nativos
**Status:** Pronto para implementação

**Relação com a v1:** este documento **substitui** `ELE3km_firmware_PRD.md`. Ele incorpora dois requisitos que a v1 não tinha (localização inercial como ferramenta explícita; o SX1276 como segundo transmissor) e as decisões da entrevista de projeto de 2026-07-28. As revogações da v1 estão listadas em Decisões de Implementação → §0.

`ELE3km_connections.md` (netlist) e `ELE3km_hardware_constraints.md` (hazards H1–H14, arbitragem C1–C6) continuam válidos e são **leitura obrigatória antes de escrever código**. Onde eles discordarem deste PRD, este vence — mas discorda em poucos pontos, todos enumerados.

---

## Problem Statement

A equipe voa um foguete a 800–1000 km/h até ~3 km de apogeu, com 3–8 km de linha de visada, numa competição universitária brasileira. Depois do voo, o foguete cai de paraquedas e frequentemente pousa em mata fechada a algumas centenas de metros da clareira. A equipe precisa de três coisas, e hoje não tem nenhuma:

1. **Recuperar o registro completo do voo** — posição, altitude, velocidade e sensores brutos — para análise e para pontuar na competição.
2. **Acompanhar o foguete ao vivo** durante subida, apogeu e descida, não só depois que ele para.
3. **Achar o foguete depois que ele pousa.** Sem uma posição confiável transmitida do chão, o foguete é perdido — e com ele o cartão SD, ou seja, os dois primeiros itens também.

A placa está montada, revisada e documentada, mas `src/main.cpp` é o boilerplate `myFunction(2,3)` do PlatformIO. Sem firmware ela é inerte.

Três problemas específicos, além do "não existe firmware":

- **O GPS vai falhar exatamente quando importa.** O NEO-6M sai de fábrica no modelo dinâmico *Portable*, que assume <1 g. Sob os 10–20 g do boost, o receptor **rejeita a própria solução** e derruba o fix; a reaquisição leva 10–60 s. A equipe perde a subida e boa parte da descida — e não tem como saber, do chão, que foi isso que aconteceu.
- **O E22 é um ponto único de falha, e uma falha silenciosa.** Sem uplink e sem ACK, o firmware é cego: RadioLib retorna sucesso com a antena solta, com o PA queimado, ou com o rail de 3,3 V afundando a cada burst de 1 A. Se o E22 parar de irradiar, a equipe não recebe nada e não descobre o motivo.
- **Uma falha isolada de módulo não pode derrubar o sistema.** IMU sem GND, sensor que faz brown-out e volta com configuração de fábrica, cartão SD que trava 250 ms — todos são cenários prováveis nesta placa, e nenhum deles pode custar a telemetria de recuperação.

## Solution

Firmware que transforma a placa ELE3km em computador de bordo. Ele lê continuamente **IMU (MPU6050)**, **barômetro (BMP280)** e **GPS (NEO-6M)**, grava tudo no **microSD** como registro durável do voo, e transmite um fluxo compacto de telemetria por **dois rádios independentes, em bandas diferentes, simultaneamente**.

Quatro decisões estruturam a solução:

**O GPS é consertado antes de ser substituído.** O firmware configura o NEO-6M no boot para o modelo dinâmico *Airborne <4g*, desliga sentenças inúteis, sobe a taxa para 5 Hz, e **reaplica a configuração sempre que detectar que o receptor resetou** — porque uma queda de rail o devolve silenciosamente para *Portable*. A localização inercial é uma **ponte curta** para vãos de 10–20 s, não navegação: com uma IMU de 6 eixos sem magnetômetro, a posição horizontal dead-reckoned diverge rápido. Passado o vão útil, o pacote passa a carregar **a última posição GPS válida mais a idade dela**, que para achar o foguete vale mais que uma posição inercial derivada.

**Os dois rádios são fail-active, não fail-over.** Ambos transmitem desde o boot, em cadências espelhadas, sem nenhuma lógica de detecção acoplando um ao outro. Isso é deliberado: as falhas do E22 que importam são invisíveis ao firmware, então um failover disparado por detecção não dispararia. Rodando os dois em paralelo, a equipe descobre a falha **pelos dados, no chão** — e ganha diversidade de recepção, porque os dois links falham por motivos independentes.

**A altitude é o payload prioritário.** Ela vem do barômetro (fundido com aceleração vertical), não do GPS, e portanto sobrevive à perda de fix. Quando a posição não é confiável, o firmware manda um pacote **só de altitude**. Altitude está presente em **todas** as transmissões.

**Nada bloqueia a amostragem dos sensores.** Duas tasks FreeRTOS em cores separados: uma amostra sensores e roda o estimador usando apenas I²C e UART; a outra é dona exclusiva do barramento SPI e absorve travadas do cartão. Toda política de arbitragem vive num escalonador puro e testável, não dentro da task.

## User Stories

### Recuperação do veículo

1. Como operador de recuperação, quero que o foguete continue transmitindo posição depois de pousar, para eu caminhar até ele na mata em vez de perdê-lo.
2. Como operador de recuperação, quero que a altitude sempre chegue, mesmo quando a posição não chega, para eu nunca ter silêncio total de telemetria.
3. Como operador de recuperação, quero **dois caminhos de rádio independentes**, em bandas e reguladores diferentes, para que uma falha de módulo, de antena ou de rail não me deixe sem nenhuma posição.
4. Como operador de recuperação, quero que a detecção de pouso exija ~1 g **mais** altitude estável perto do solo, para que a queda livre no apogeu nunca seja confundida com pouso.
5. Como operador de recuperação, quero que o modo beacon use um fator de espalhamento alto e cadência lenta, para maximizar a chance de ser ouvido embaixo de vegetação.
6. Como operador de recuperação, quero que a posição transmitida depois do pouso seja uma **média de fixes bons acumulados**, não o último fix recebido, porque sob copa de árvore o último costuma ser o pior.
7. Como operador de recuperação, quero saber a **qualidade** dessa posição — número de amostras, HDOP, idade — para eu saber se caminho para um ponto ou para um círculo de 100 m.
8. Como operador de recuperação, quero que, sem nenhum fix pós-pouso, o foguete transmita a **última posição válida de voo** com a altitude barométrica da descida, porque um fix a 200 m na descida limita o ponto de pouso a um raio pequeno.
9. Como operador de recuperação, quero que o beacon continue por horas com a bateria disponível, porque a busca pode durar mais que o voo.

### Estação de solo

10. Como operador da estação de solo, quero telemetria ao vivo a ~1 Hz durante **todo** o voo — rampa, boost, coast, descida e pouso — não só depois do toque.
11. Como operador da estação de solo, quero que cada pacote indique se a posição veio do GPS ou do fallback inercial, para eu saber quanto confiar nela.
12. Como operador da estação de solo, quero um número de sequência e um tempo em cada pacote, para eu detectar pacotes perdidos e ordená-los.
13. Como operador da estação de solo, quero que **o mesmo número de sequência** apareça nos dois rádios, para eu deduplicar e montar uma trajetória única a partir dos dois fluxos.
14. Como operador da estação de solo, quero um formato de pacote fixo e documentado (completo e só-altitude), para eu escrever o receptor sem adivinhar.
15. Como operador da estação de solo, quero ver a saúde de cada subsistema em cada pacote, para eu diagnosticar do chão o que falhou a bordo.
16. Como operador da estação de solo, quero ver a fase de voo em cada pacote, para eu perceber imediatamente uma transição de fase indevida.
17. Como operador da estação de solo, quero que as duas transmissões sejam defasadas no tempo, para os dois receptores não competirem pela mesma janela.

### Análise de dados de voo

18. Como analista, quero cada leitura bruta de sensor (accel, gyro, pressão, temperatura, GPS) gravada no microSD com timestamp, para eu reconstruir o voo inteiro depois.
19. Como analista, quero as saídas fundidas do estimador (posição, altitude, velocidade vertical, fonte de posição, fase) gravadas ao lado dos brutos, para eu avaliar como o filtro se comportou.
20. Como analista, quero que uma queda de energia ou um travamento em pleno voo me custe apenas a **cauda** do log, não o arquivo inteiro.
21. Como analista, quero que os registros tenham tamanho fixo, com marcador mágico, sequência e CRC, para uma ferramenta pós-voo recuperar todo registro válido varrendo o arquivo bruto.
22. Como analista, quero o **contador de boot em cada registro**, para que registros de um voo anterior — que podem ocupar os mesmos clusters e validar magic+CRC perfeitamente — não contaminem o voo atual.
23. Como analista, quero um arquivo novo por boot, para que um reset em voo nunca sobrescreva o anterior.
24. Como analista, quero timestamps tirados no instante da **aquisição**, nunca no instante da escrita.
25. Como analista, quero a pressão bruta gravada ao lado da altitude derivada, para eu poder rederivar a altitude com outro modelo depois.
26. Como analista, quero um contador de reinicializações por sensor no log, porque se ele subir durante o voo é sinal de que o rail está afundando e a rev. 2 precisa de um buck separado.
27. Como analista, quero amostras de IMU que bateram no fundo de escala marcadas como **saturadas**, para que dados de boost clipados não sejam integrados como válidos.
28. Como analista, quero que a estação de solo grave todo pacote recebido, bruto e com timestamp de recepção, para eu ter um registro do voo mesmo que o foguete ou o cartão nunca sejam recuperados.
29. Como analista, quero que a taxa de log caia depois do pouso confirmado, para o beacon não gravar horas de dados parados na taxa de voo.

### GPS e fonte de posição

30. Como computador de bordo, quero configurar o NEO-6M para o modelo dinâmico **Airborne <4g** no boot, para que ele não rejeite a própria solução sob a aceleração do boost.
31. Como computador de bordo, quero desligar as sentenças NMEA que não uso e subir a taxa para 5 Hz, para ter fixes mais frequentes a 278 m/s.
32. Como computador de bordo, quero **salvar a configuração UBX na memória com bateria de backup**, para que um reset do receptor volte já configurado.
33. Como computador de bordo, quero **detectar que o GPS resetou** (fluxo NMEA reiniciando, ou satélites caindo a zero logo após uma transmissão) e reaplicar a configuração inteira, porque senão ele volta em *Portable* silenciosamente.
34. Como computador de bordo, quero rodar um filtro de Kalman fundindo GPS com movimento integrado do acelerômetro, para que a posição transmitida seja mais suave e robusta que o GPS bruto.
35. Como computador de bordo, quando o fix é válido, quero que o passo de correção do GPS zere continuamente a deriva inercial.
36. Como computador de bordo, quando o fix se perde, quero propagar a posição por integração inercial durante uma janela curta e configurável, para continuar transmitindo *uma* posição durante o vão.
37. Como computador de bordo, passada essa janela, quero parar de transmitir posição inercial e passar a transmitir **a última posição GPS válida mais a idade dela**, porque isso é mais útil e mais honesto.
38. Como computador de bordo, quando o fix retorna, quero que a estimativa reconverja para o GPS.
39. Como computador de bordo, ao entrar em pouso confirmado, quero reconfigurar o GPS para o modelo **Stationary**, porque o modelo Airborne impede a filtragem pesada que dá boa precisão parado.

### Altitude

40. Como computador de bordo, quero calcular a altitude primariamente do barômetro, fundida com aceleração vertical, para que ela não dependa do GPS.
41. Como computador de bordo, quero **variar o peso do barômetro em função da velocidade estimada** — peso alto abaixo de ~30 m/s, quase nulo acima de ~100 m/s — porque a leitura barométrica é ruim em alta velocidade.
42. Como computador de bordo, quero que durante o boost a altitude venha da integração vertical do acelerômetro, que nesse eixo é confiável porque a gravidade dá referência de atitude.
43. Como computador de bordo, quero rejeitar degraus impossíveis de pressão por limite de taxa de variação, em vez de alimentá-los ao filtro.
44. Como computador de bordo, se o barômetro estiver ausente, quero cair para a altitude do GPS e sinalizar isso no pacote.
45. Como integrante da equipe de estrutura, quero uma especificação de **porta estática** (número, diâmetro e posição dos furos), porque nenhum tratamento em software corrige pressão de estagnação vazando para o bay.

### Fases de voo e referência barométrica

46. Como computador de bordo, enquanto estou na rampa, quero manter uma **média lenta contínua** da pressão ambiente, porque uma referência tirada no power-on está velha na hora da ignição.
47. Como computador de bordo, quero detectar o liftoff por **aceleração** (|a| acima de um limiar, sustentada), não por altitude, porque a altitude é justamente o que ainda não posso medir.
48. Como computador de bordo, no instante do liftoff quero **congelar a referência no valor de ~1 s antes**, lido de um anel de histórico, para não contaminá-la com o transiente do motor nem com manuseio na rampa.
49. Como computador de bordo, quero persistir a referência congelada, a fase e o contador de boot em memória não-volátil, escrevendo poucas vezes por voo para não destruir o flash.
50. Como computador de bordo, ao voltar de um reset, quero reusar a referência salva **apenas se** o motivo do reset não foi um power-on limpo **e** a fase salva era de voo, para que um brown-out na rampa não me faça voar com referência velha.
51. Como computador de bordo, se a referência reusada implicar uma altitude absurda, quero cair para a altitude do GPS e sinalizar isso no pacote.
52. Como computador de bordo, quero exigir **quatro condições simultâneas** para declarar pouso: aceleração perto de 1 g, altitude estável numa faixa por N segundos, altitude perto da referência de solo, e tempo mínimo desde o liftoff.
53. Como computador de bordo, quero que a transição para pouso seja **de mão única** — só um reset sai dela.
54. Como computador de bordo, quero registrar a fase de voo em todo registro de log e em todo pacote.

### Rádio e transmissão

55. Como computador de bordo, quero que o E22 transmita a 1 Hz durante todo o voo, num fator de espalhamento cujo airtime caiba num ciclo de trabalho seguro para o PA e para o rail.
56. Como computador de bordo, quero que o SX1276 transmita **em paralelo, a 1 Hz, com o mesmo número de sequência, defasado ~500 ms**, para dar diversidade de recepção sem sobrepor os dois consumos no nó da bateria.
57. Como computador de bordo, quero que os dois rádios operem **sem nenhuma lógica de detecção de falha ligando um ao outro**, porque as falhas do E22 que importam não são detectáveis a bordo.
58. Como computador de bordo, quero operar o SX1276 na faixa livre brasileira de 915–928 MHz, não no default de 868 dos exemplos.
59. Como computador de bordo, quero um **teto de ciclo de trabalho verificado em código** limitando as transmissões numa janela deslizante, que a lógica de cadência não pode furar, seja qual for a fase.
60. Como computador de bordo, quero tentar um pacote **completo** quando a posição é confiável e um pacote **só de altitude** quando não é, com altitude presente nos dois.
61. Como computador de bordo, quero usar transmissão não-bloqueante com interrupção de fim de transmissão, para que o airtime não pare a task de I/O.
62. Como computador de bordo, quero **nunca confiar numa borda de interrupção do rádio sozinha** — sempre confirmar contra o registrador de status de IRQ — por causa da errata dos pinos de entrada usados.
63. Como computador de bordo, quero **nunca esperar indefinidamente** pelo sinal de ocupado do rádio: timeout limitado mais caminho de recuperação por reset.
64. Como computador de bordo, quero manter WiFi e Bluetooth desabilitados explicitamente, porque não uso nenhum dos dois e ambos custam corrente em rajadas e interrupções de CPU.
65. Como computador de bordo, quero que depois do pouso os dois rádios divirjam: o de 433 MHz vai para máxima sensibilidade e cadência lenta, o de 915 MHz mantém uma cadência intermediária.

### Robustez e degradação

66. Como computador de bordo, quero detectar cada subsistema no startup e registrar quais estão saudáveis.
67. Como computador de bordo, quero que cada driver exponha os mesmos três estados de saúde e que **toda** operação tenha timeout duro.
68. Como computador de bordo, quero retentar um módulo marcado como falho a cada 5 s, indefinidamente, porque uma falha transitória no lift-off não pode custar o sensor pelo voo inteiro.
69. Como computador de bordo, quero uma **rotina de recuperação do barramento I²C** (pulsos de clock para destravar um escravo que segura a linha), porque um único escravo travado leva a IMU **e** o barômetro juntos — e com eles a altitude.
70. Como computador de bordo, quero **detectar o barômetro antes da IMU** no startup, para que, se a IMU travar o barramento, eu ao menos saiba que o barômetro estava saudável.
71. Como computador de bordo, quero **reverificar periodicamente os registradores de configuração** dos sensores, não só os de dados, porque um sensor que fez brown-out e voltou responde normalmente e devolve dados plausíveis com a escala errada.
72. Como computador de bordo, quero limitar o custo de tempo das tentativas de recuperação — no máximo um módulo por ciclo, com timeout de barramento explícito e baixo — para que elas não comam o orçamento do ciclo.
73. Como computador de bordo, se a IMU estiver ausente ou morta, quero desabilitar o fallback inercial e continuar em GPS + barômetro.
74. Como computador de bordo, se o GPS ainda não tiver fix, quero continuar gravando e transmitindo altitude.
75. Como computador de bordo, se o barômetro estiver ausente, quero cair para altitude do GPS.
76. Como computador de bordo, se o microSD falhar ou estiver ausente, quero continuar transmitindo — uma falha de log nunca pode calar o link de recuperação.
77. Como computador de bordo, quero um watchdog armado em cada task, para que um driver travado vire um reboot com estado preservado em vez de um foguete mudo.

### Concorrência e arbitragem

78. Como computador de bordo, quero amostrar os sensores continuamente, sem pausa, mesmo durante uma travada de 250 ms do cartão SD, porque uma lacuna nas amostras corrompe sistematicamente o passo de predição do filtro.
79. Como computador de bordo, quero que a task de sensores use apenas I²C e UART, nunca SPI, para que ela nunca dispute barramento com o log nem com os rádios.
80. Como computador de bordo, quero um único dono do barramento SPI, garantindo que exatamente um dos três chip-selects esteja ativo por vez.
81. Como computador de bordo, quero que **nenhuma escrita no cartão comece enquanto o PA de 1 W estiver ligado** — regra dura, não melhor-esforço — porque o pico de corrente e a escrita compartilham o mesmo rail.
82. Como computador de bordo, quero que uma solicitação de transmissão **nunca interrompa** uma escrita em andamento.
83. Como computador de bordo, quero que o airtime do rádio de 915 MHz **não bloqueie** escrita no cartão, porque ele não está no rail disputado — a regra de exclusão vale só para o de 1 W.
84. Como computador de bordo, quero um buffer circular em RAM entre a produção e a gravação, dimensionado para cobrir a maior janela de rádio somada à pior travada de cartão.
85. Como computador de bordo, em caso de estouro do buffer quero **descartar os registros mais antigos**, nunca atrasar uma leitura de sensor nem um pacote de telemetria.
86. Como computador de bordo, quero que os estados de arbitragem sejam eventos de duração variável, não fatias fixas de tempo, para que uma travada de cartão degrade a resolução do log em vez de quebrar o ciclo.

### Desenvolvimento e teste

87. Como desenvolvedor, quero o filtro, a máquina de fonte de posição, a máquina de fases e o codec de pacotes num núcleo puro sem dependência de hardware, para eu testá-los no notebook.
88. Como desenvolvedor, quero **o escalonador de arbitragem também dentro desse núcleo puro**, para que as invariantes de exclusão e de descarte sejam exercidas em teste e não só voando.
89. Como desenvolvedor, quero que o núcleo puro não use nenhuma API de Arduino, nenhum relógio global e nenhuma variável global — o tempo entra como parâmetro — para que os testes sejam determinísticos.
90. Como desenvolvedor, quero alimentar sequências sintéticas de amostras no núcleo e verificar os registros e pacotes emitidos, sem voar.
91. Como desenvolvedor, quero um **harness de replay** que reproduza um log de voo real através do núcleo no host, para que o primeiro voo vire o teste de regressão de todas as versões futuras do estimador.
92. Como desenvolvedor, quero a garantia escrita de que o registro de log é **suficiente para reconstruir a amostra de sensores que o produziu**, para que ninguém remova um campo bruto no futuro alegando que ele já está na saída fundida.
93. Como desenvolvedor, quero os drivers de hardware isolados atrás de uma HAL fina, para que trocar um sensor ou um rádio não se propague para a lógica de voo.
94. Como desenvolvedor, quero o mapa de pinos definido em exatamente um lugar, batendo com o netlist real, para que firmware e hardware nunca divirjam.
95. Como desenvolvedor, quero um ambiente de build nativo com framework de teste, além do ambiente de target.
96. Como desenvolvedor, quero regras de disciplina explícitas e verificáveis em revisão de código para os pinos que podem destruir hardware.

## Implementation Decisions

### §0 — Revogações da v1

| # | O que a v1 / os docs de hardware dizem | O que passa a valer | Por quê |
|---|---|---|---|
| R1 | *"The onboard SX1276 is never initialized for transmit"* | O SX1276 **transmite**, em 915–928 MHz, em paralelo | Redundância real: o SX1276 vive no regulador interno do Heltec (buck de 5 V); o E22 no rail de 3,3 V (buck separado). Reguladores, chips e antenas diferentes |
| R2 | `PIN_LORA_CS = 18` "drive HIGH forever" | GPIO18 é um chip-select normal. O passo de boot (HIGH antes de `SPI.begin()`) **continua obrigatório** | O hazard H2 continua válido — o pino flutua no reset e o rádio se auto-seleciona. Só deixa de ser permanente |
| R3 | `PIN_LORA_RST = 14` mantido em LOW | GPIO14 é liberado — reset do SX1276, controlado pelo driver | O rádio precisa funcionar |
| R4 | "Nenhuma escrita no SD durante TX" | Vale **apenas para o E22**. O airtime do SX1276 não bloqueia o SD | A regra protege o **rail**, não o barramento — o próprio §C3 diz isso. O SX1276 não está nesse rail |
| R5 | "GPIO18 não faz parte da arbitragem" | GPIO18 **entra** na arbitragem como terceiro chip-select | Consequência direta de R1 |
| R6 | Estação de solo com um receptor | **Dois receptores**, um por banda, cada um com log bruto | Consequência direta de R1 |

### §1 — Módulos

- **`core/` — puro, sem hardware, testável no host.** Contrato duro: **nenhum header de Arduino, nenhuma leitura de relógio global, nenhuma variável global.** O tempo entra como parâmetro em toda API.
  - **Estimador** — filtro de Kalman com canal horizontal (posição + velocidade) e canal vertical (altitude + velocidade vertical). Predição pela aceleração da IMU; correção pelo GPS (horizontal e altitude de GPS) e pelo barômetro (vertical). Expõe a estimativa corrente, a covariância por canal e a fonte de posição ativa. O **R do barômetro é função da velocidade estimada**.
  - **Máquina de fonte de posição** — GPS válido → fonte GPS; GPS perdido além de um limiar → fonte INS (só predição); além da janela de ponte → fonte "última válida" carregando a idade do fix; GPS reaquirido → volta a GPS e reconverge. Emite a flag de confiança que decide pacote completo vs. só-altitude.
  - **Máquina de fases de voo** — RAMPA → VOO → POUSADO, com a detecção de liftoff por aceleração e a detecção de pouso pelas quatro condições. Transição para POUSADO é de mão única.
  - **Codec de telemetria** — serializa as duas formas de pacote em layout binário fixo, little-endian, documentado.
  - **Codec de log** — serializa e desserializa o registro de tamanho fixo; a função de varredura/recuperação vive aqui e é testada aqui.
  - **Escalonador de arbitragem** — recebe estado + tempo, devolve a próxima ação. Aqui vivem a regra de exclusão rail-vs-escrita, a política de descarte no estouro do buffer, e a decisão de qual rádio transmite quando. **Nenhuma dessas políticas pode viver na task.**
  - **FlightComputer** — o orquestrador: `update(SensorSample, t) → { LogRecord, lista de TelemetryPacket }`. É o seam primário de teste.
- **`hal/` — adaptadores finos, só no target:** MPU6050 (I²C), BMP280 (I²C), NEO-6M (UART, NMEA + configuração UBX), microSD (SPI, biblioteca com pré-alocação), rádio SX1268 externo (SPI), rádio SX1276 onboard (SPI), persistência não-volátil.
- **`src/main.cpp` e as tasks** — ligam a HAL ao núcleo. **Bombas burras:** executam as ações que o escalonador decidiu, sem política própria.

### §2 — Concorrência

Duas tasks FreeRTOS, fixadas em cores diferentes, com buffers circulares entre elas.

| Task | Core | Prioridade | Responsabilidade | Barramentos |
|---|---|---|---|---|
| `flight` | 1 | alta | Poll da IMU e do barômetro, drenagem da UART do GPS, estimador, decisões | **só I²C e UART** |
| `io` | 0 | média | Dono exclusivo do SPI: cartão + os dois rádios, arbitragem de chip-select | **só SPI** |

**Motivo:** um microSD para de responder por 100–250 ms durante coleta de lixo interna — comportamento normal, sem erro. Num superloop isso engole as amostras da IMU e o passo de predição integra aceleração com um Δt errado, sistematicamente, durante o boost. Com tasks separadas a travada fica no core 0 e o core 1 não percebe.

Transmissão não-bloqueante com IRQ. Watchdog de task armado nas duas. WiFi e Bluetooth desabilitados explicitamente.

### §3 — Taxa de ciclo

| Item | Taxa |
|---|---|
| Ciclo, Δt do filtro, taxa de log | **100 Hz** (Δt = 10 ms) |
| IMU | 100 Hz |
| Barômetro | 25 Hz |
| GPS | 5 Hz |

Orçamento de I²C a 100 kHz: ~15% para a IMU, ~2% para o barômetro, ~17% total — com folga para retries e recuperação de barramento. A 200 Hz seriam ~32% num barramento que os docs de hardware já marcam como frágil, e o firmware **não pode subir para 400 kHz** (o pino de dados é também o controle de Vext do módulo).

Um registro de 64 B por ciclo dá 6,4 kB/s e **8 registros por bloco de 512 B** — é essa a razão do tamanho 64. Depois do pouso confirmado a taxa de log cai para 1 Hz.

### §4 — GPS

Configuração enviada no boot e **reaplicada ao detectar reset do receptor**:

- Modelo dinâmico **Airborne <4g**
- Sentenças não usadas desligadas; taxa elevada para 5 Hz
- Configuração salva na memória com bateria de backup, para sobreviver a um reset por queda de rail
- Ao entrar em POUSADO: modelo dinâmico trocado para **Stationary**

Detecção de reset: fluxo NMEA reiniciando, ou contagem de satélites caindo a zero logo após uma transmissão.

O limite CoCom (velocidade **e** altitude simultaneamente altas) não se aplica nesta trajetória — a velocidade fica bem abaixo do limiar.

### §5 — Dois caminhos de rádio

| | E22 / 433 MHz | SX1276 / 915 MHz |
|---|---|---|
| Potência | +30 dBm | +20 dBm |
| Regulador | buck de 3,3 V (compartilhado com sensores e SD) | regulador interno do Heltec (buck de 5 V) |
| Recurso escasso | **airtime e rail** | praticamente nenhum |
| Cadência em voo | 1 Hz | 1 Hz, defasado ~500 ms |
| Cadência pós-pouso | lenta, máxima sensibilidade | intermediária |
| Ponto forte | penetra folhagem; beacon primário de recuperação | banda folgada; independência de rail |

**Fail-active:** os dois transmitem desde o boot, sem nenhuma lógica de detecção acoplando-os. **Número de sequência global único**, compartilhado.

**A defasagem de 500 ms tem duas justificativas:** não empilhar as duas transmissões na mesma janela da task de I/O, e não somar os dois consumos no nó de bateria compartilhado pelos dois bucks. O escalonador **verifica** essa invariante, não a presume.

**Faixa:** 915–928 MHz. Não 868 — essa faixa não é livre no Brasil, e é o default de metade dos exemplos de biblioteca.

**O que a redundância cobre e o que não cobre** — registrado explicitamente porque a nota de projeto original levantou a objeção:

| Modo de falha | Coberto? |
|---|---|
| E22 morto, DOA, PA queimado | ✅ chip diferente |
| Rail de 3,3 V afundando sob o pico de 1 A | ✅ reguladores diferentes |
| Antena do E22 solta ou danificada | ✅ antenas diferentes |
| Bug de driver ou de SPI específico do E22 | ✅ driver e banda diferentes |
| **ESP32 travando ou resetando** | ❌ mesmo MCU — mitigado só pelo watchdog |
| **Bateria acabando ou interruptor abrindo** | ❌ mesmo nó — mitigação é de montagem, não de firmware |

### §6 — Formato de pacote

**Airtime LoRa é quantizada em blocos de 8 símbolos.** No fator de espalhamento escolhido para o voo isso são ~16 ms por degrau, e o teto do bucket-alvo é **21 bytes**. O pacote foi dimensionado para caber em **20 bytes exatos**, preservando sem alteração a tabela de airtime e o orçamento de bateria já calculados nos docs de hardware.

**O CRC de aplicação foi removido do pacote de rádio** — o LoRa já tem CRC de hardware e o driver não entrega pacote reprovado. No registro do cartão o CRC **permanece**: ali não existe camada de enlace.

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

**Pacote só-altitude — 12 B:** idêntico, sem latitude e longitude. **Altitude está presente nas duas formas.**

Os dois rádios enviam o **mesmo formato e o mesmo conteúdo**. Um codec, um conjunto de testes, e a estação de solo trata os dois receptores de forma idêntica.

### §7 — Arbitragem de recursos

O recurso escasso é o **rail**, não o barramento: a carga útil é escrita no rádio antes de a transmissão ser disparada, e durante o airtime o rádio transmite sozinho com o barramento livre.

| Recurso | Quem disputa |
|---|---|
| **Rail de 3,3 V** (~1 A) | E22 transmitindo **⊕** escrita no cartão. Regra dura |
| **Barramento SPI** | cartão **⊕** E22 (carga de payload) **⊕** SX1276 (carga de payload). Três chip-selects: exatamente um ativo por vez |
| **Nó da bateria** | transmissão do E22 **⊕** transmissão do SX1276 — resolvido pelo agendamento defasado, verificado pelo escalonador |

Consequências: o **airtime do SX1276 não bloqueia nada**; a **carga de payload do SX1276** é uma transação curta agendada entre blocos de escrita; uma solicitação de transmissão nunca interrompe uma escrita em andamento; o buffer circular absorve o jitter do cartão; no estouro, descarta-se o registro mais antigo, nunca uma leitura de sensor nem um pacote.

### §8 — Logging

- Registros binários de **tamanho fixo (64 B)**, cada um com marcador mágico, número de sequência, **contador de boot** e CRC.
- Arquivo **pré-alocado e contíguo**; a tabela de alocação **não é tocada durante o voo**; metadados escritos exatamente duas vezes (criação e fechamento). Isso remove o modo de falha que destrói o arquivo inteiro, em vez de tentar agendar em volta dele.
- Biblioteca de cartão com suporte a pré-alocação e arquivo contíguo — a biblioteca padrão do Arduino não serve, porque não suporta nenhum dos dois, que são o mecanismo inteiro.
- Bloco de cabeçalho no offset 0: magic, versão do formato, contador de boot, pressão de referência de solo, revisão do mapa de pinos.
- **Arquivo novo por boot**, com índice incremental.
- Escritas alinhadas em blocos de 512 B. Timestamps na **aquisição**.
- O **contador de boot vai em cada registro**, não só no cabeçalho: o arquivo pré-alocado pode cair nos clusters do voo anterior, e como o recuperador varre por magic + CRC, registros antigos validam perfeitamente e entram no meio do voo atual.

### §9 — Referência barométrica e fases

- Em RAMPA: média lenta contínua da pressão (constante de tempo de dezenas de segundos) mais um anel curto de histórico.
- **Liftoff** por aceleração sustentada acima de um limiar por ~100 ms. Não por altitude.
- No liftoff a referência é **congelada no valor de ~1 s antes**, lido do anel.
- Persistida em memória não-volátil nesse instante e nas transições de fase — poucas escritas por voo.
- **Ao voltar de um reset:** reusa a referência salva apenas se o motivo do reset não foi power-on **e** a fase salva era de voo. Sanidade: se a referência reusada implicar altitude absurda, cai para altitude de GPS e sinaliza no pacote.
- **POUSADO exige as quatro condições simultâneas.** Em queda livre o acelerômetro lê ≈0 g **de forma estável**; em repouso lê ≈1 g. Um detector baseado em baixa variância dispara no apogeu e joga fora a telemetria da descida inteira — que é a razão de existir deste PRD.

### §10 — Altitude e porta estática

Firmware: R do barômetro variável com a velocidade estimada; rejeição de degraus de pressão por limite de taxa; filtro interno do sensor habilitado com consciência do atraso que ele adiciona; pressão bruta logada ao lado da altitude derivada; amostras de IMU no fundo de escala marcadas como saturadas e excluídas ou com covariância inflada no passo de predição.

**Requisito mecânico (não é firmware, mas o firmware depende dele):** porta estática com **3 ou 4 furos de ~2 mm igualmente espaçados na circunferência** (a simetria cancela erro de ângulo de ataque e o bombeamento pelo rolamento — é a parte que realmente conta), a pelo menos um diâmetro de corpo de qualquer mudança de geometria e um diâmetro à frente das aletas, sem rebarba, furo perpendicular à pele.

O dimensionamento tem folga enorme: o erro de atraso em regime permanente cresce com o **quadrado** da vazão necessária e cai com o quadrado da área, e para um bay típico com três furos de 2 mm dá ordem de centímetros na velocidade máxima. Além disso o erro é **proporcional à velocidade**, e no apogeu a velocidade é zero — o número que a competição pontua sai praticamente livre de atraso. O risco real não é furo pequeno demais, é furo **assimétrico** ou bay selado.

### §11 — Saúde e degradação

- Cada driver expõe `{OK, DEGRADED, FAILED}`; toda operação com timeout duro.
- Módulo em `FAILED` retentado a cada **5 s**, período fixo. (Backoff exponencial com teto de 5 s chegaria ao teto em três tentativas num voo de ~200 s — mesma coisa com mais estado para testar.)
- **Recuperação de barramento I²C** obrigatória: soltar a linha de dados, pulsar o clock ~9 vezes, emitir parada, reinicializar o driver. Sem ela, a reinicialização a cada 5 s falha para sempre.
- **Ordem de probe:** barômetro **primeiro**.
- **Reverificação periódica dos registradores de configuração** no caminho saudável.
- Contador de reinicializações por sensor no log.
- Orçamento de tempo: timeout de barramento explícito e baixo; no máximo um módulo tentado por ciclo.
- Estado de saúde de cada subsistema em todo registro e em todo pacote.

### §12 — Disciplina (custa zero código; primeiro commit)

| Regra | Consequência de violar |
|---|---|
| **Nenhum código de LED em lugar nenhum** — o pino do LED do módulo é o habilitador de transmissão | Piscar o LED **liga o PA de 1 W** |
| **Só a biblioteca de rádio toca os pinos do comutador de RF**; nunca escrita direta | Os dois habilitadores altos ao mesmo tempo roteiam saída do PA para o LNA — **módulo destruído** |
| **Nunca acionar o pino de strapping antes do boot terminar**; não montar pull-up externo nele | A placa **não dá boot** |
| **Não usar ADC1, sensor Hall, touch nem coprocessador de baixo consumo** | Errata do MCU: os pinos de entrada usados pelo rádio registram pulso espúrio quando o domínio analógico chaveia ⇒ IRQ fantasma |
| **Nunca confiar numa borda de IRQ sozinha** — confirmar contra o registrador de status | "Rádio pronto" falso |
| **Nunca esperar indefinidamente no sinal de ocupado** — timeout + recuperação | Travamento permanente da task de I/O |
| **Não cortar a alimentação do rádio externo entre transmissões** | Não há chave de carga na placa; o standby já é ~2 mA; o pino de reset não tem pull-up |
| **Nunca transmitir sem antena conectada** (vale para os dois rádios) | 1 W em circuito aberto **destrói o PA** |

### §13 — Contrato de pinos

Vale o contrato do documento de conexões, com o delta de que o rádio onboard passa a ser ativo: o chip-select dele vira um chip-select normal (mas o passo de boot que o coloca em nível alto **antes** de inicializar o barramento continua obrigatório), e o pino de reset dele passa a ser controlado pela biblioteca de rádio em vez de mantido em nível baixo. Os pinos de interrupção do rádio onboard são internos ao módulo e estão disponíveis.

Ordem de boot, inalterada e obrigatória: desselecionar o rádio onboard, desselecionar o cartão, desselecionar o rádio externo — **todos antes de inicializar o barramento SPI** — e não tocar no pino de strapping. **Três chip-selects vivos no barramento; exatamente um ativo por vez.**

## Testing Decisions

### O que faz um bom teste aqui

Exercita **comportamento externo** do núcleo, nunca detalhes internos. Dado um fluxo de amostras de entrada, verifica os registros de log e os pacotes emitidos — **nunca** matrizes privadas do filtro nem variáveis intermediárias. Todo teste é determinístico: o tempo entra como parâmetro, não é lido de relógio.

### Seams

Ordenados do mais alto para o mais baixo. Preferir sempre o mais alto que sirva.

1. **`FlightComputer::update`** — o seam primário. Recebe uma amostra e o tempo, devolve um registro e uma lista de pacotes. A maioria dos testes entra por aqui.
2. **Escalonador de arbitragem** — recebe estado e tempo, devolve a próxima ação. É o seam que torna as invariantes de recurso testáveis; **é a razão de o escalonador estar no núcleo e não na task.**
3. **Máquina de fases de voo** — alimentada com um perfil sintético de trajetória.
4. **Máquina de fonte de posição** — alimentada com sequências de validade/invalidade de fix.
5. **Estimador** — dirigido por trajetória sintética conhecida mais ruído.
6. **Codecs (telemetria e log)** — ida e volta puros.

### Testes por seam

**Máquina de fases — o teste de maior valor da suíte.** Alimentar um perfil sintético de voo completo (rampa → boost → coast → **queda livre no apogeu** → descida → toque) e verificar que a fase permanece em VOO durante o apogeu. Queda livre lê ≈0 g **de forma estável**; repouso lê ≈1 g. Essa distinção é o teste inteiro — é ele que pega um detector de pouso construído sobre baixa variância. Verificar também: POUSADO só é atingido com as quatro condições, POUSADO nunca é abandonado, e os pacotes continuam na cadência de voo durante toda a descida.

**FlightComputer.** Altitude aparece em **todo** pacote emitido. Pacote completo enquanto o GPS é válido; pacote só-altitude quando a confiança cai. Passada a janela de ponte inercial, o pacote passa a carregar última posição válida com idade, e não posição inercial derivada. Sequências incrementam e a cadência bate com a configurada. Com a IMU marcada como ausente, nenhum fallback inercial é tentado e a telemetria de GPS + barômetro continua fluindo.

**Escalonador.** Com rádio e cartão falsos: nenhuma escrita é emitida enquanto o PA de 1 W está ligado; uma travada de cartão que atravessa vários ciclos descarta os registros mais antigos em vez de atrasar um pacote; uma solicitação de transmissão espera uma escrita em andamento em vez de interrompê-la; o airtime do rádio de 915 MHz **não** bloqueia escrita; as duas transmissões nunca se sobrepõem; o teto de ciclo de trabalho nunca é furado, mesmo quando a lógica de cadência pede mais.

**Estimador.** Dirigido por trajetória sintética com ruído: a estimativa acompanha dentro de erro limitado; a correção de GPS reduz o erro; durante um vão de GPS ele roda só em predição e a incerteza de posição cresce. Com velocidade alta, o peso do barômetro cai — verificado pelo comportamento da saída, não pela matriz.

**Fonte de posição.** GPS válido → fonte GPS. N amostras obsoletas → fonte INS. Além da janela → fonte "última válida" com idade correta. GPS retorna → fonte GPS e reconvergência.

**Codec de telemetria.** Ida e volta nas duas formas. Altitude decodifica corretamente do pacote só-altitude. **Os tamanhos codificados são exatamente 20 B e 12 B** — este teste protege o orçamento de airtime e de ciclo de trabalho, que é a razão de o formato ser o que é.

**Codec de log e recuperação.** Um fluxo de bytes truncado num offset arbitrário ainda entrega todo registro completo quando varrido. Registros com CRC corrompido são rejeitados, não aceitos silenciosamente. **Registros com contador de boot diferente do cabeçalho são descartados** — este é o teste que impede contaminação por um voo anterior nos mesmos clusters.

**Saturação.** Amostras de IMU no fundo de escala são marcadas e excluídas ou fortemente atenuadas no passo de predição.

**Referência barométrica.** Simular um reset em pleno voo e verificar que a referência salva é reusada e a altitude reportada fica contínua. Simular um power-on limpo e verificar que ela **não** é reusada. Simular liftoff e verificar que a referência congelada é a de ~1 s antes, não a do instante do transiente.

**Harness de replay — o item de maior valor de longo prazo.** O registro de 64 B carrega os valores **brutos** *e* os fundidos, então um log de voo real pode ser reproduzido pelo núcleo no host, comparando as saídas. O log do primeiro voo vira o teste de regressão de todas as versões futuras do estimador — um caso de teste que nenhum dado sintético iguala.

> **Requisito escrito, para ninguém "otimizar" depois:** o registro de log deve ser suficiente para reconstruir a amostra de sensores que o produziu. Nenhum campo bruto pode ser removido sob o argumento de que já está na saída fundida.

### Adaptadores de hardware

Verificados no target (gravar e observar), não em testes nativos. O checklist de auto-teste de startup do documento de conexões é o roteiro dessa verificação.

### Prior art

Nenhuma — o projeto é greenfield. Este PRD estabelece o padrão: núcleo puro sob testes nativos, HAL excluída dos testes unitários.

## Out of Scope

- **Firmware da estação de solo** (decodificação e exibição) — esforço separado; este PRD define o formato que ela precisa consumir. **Dois requisitos são impostos a ela e não são opcionais:** precisa de **dois receptores**, um por banda, e precisa **gravar todo pacote recebido, bruto e com timestamp**, em disco. Esse log é o seguro do projeto contra perder o veículo, então é dependência deste PRD, não algo fora dele.
- **Navegação inercial de precisão ou de longa duração** — a IMU de 6 eixos não sustenta isso. O INS é ponte curta e nada mais.
- **Display OLED** — fisicamente indisponível nesta placa (o pino foi reaproveitado).
- **Acionamento de recuperação** (paraquedas, pirotecnia) — não é função desta placa.
- **Uplink e comandos da estação de solo para o foguete** — considerado e descartado; ambos os rádios são só transmissores.
- **Correções de hardware** — o jumper de GND da IMU, o tipo de módulo de cartão, o ajuste dos trimpots e a porta estática são ações de montagem. O firmware **detecta e degrada**; ele não conserta fiação.
- **Cobertura das falhas de MCU e de bateria** — a redundância de rádio não as cobre (mesmo processador, mesmo nó de bateria). Mitigação é watchdog no firmware e margem de bateria na montagem.
- **Seleção regulatória de frequência além das faixas escolhidas** — 433 MHz é fixado pelo hardware do rádio externo; 915–928 MHz é a faixa livre nacional para o rádio onboard.

## Further Notes

**Limitação honesta do fallback inercial.** Com uma IMU de 6 eixos sem magnetômetro, a posição horizontal dead-reckoned degrada em segundos a dezenas de segundos. Isso é aceitável porque (a) a altitude — o payload prioritário — é barométrica e não é afetada, e (b) a função do fallback é continuidade durante vãos curtos, não navegação autônoma. **A equipe de solo precisa saber disso** para confiar mais nos fixes de origem GPS do que nos de origem inercial — daí a flag de fonte em todo pacote.

**A assimetria que estrutura o projeto.** O mesmo sensor inercial é excelente num eixo e inútil no outro: **verticalmente** a integração é confiável, porque a gravidade dá referência de atitude; **horizontalmente** é lixo, porque sem magnetômetro o heading não é observável. É exatamente por isso que a altitude é o payload prioritário e a posição não é — e por que a solução para "perdi o GPS" é consertar o GPS, não construir um substituto inercial.

**A causa raiz do modo de falha mais provável.** O documento de hazards observa que a IMU tem o GND flutuando e que, nesse estado, ela é alimentada parasiticamente pelos diodos de proteção através dos pull-ups do barramento. Uma peça assim não "deixa de responder" limpo — ela **segura a linha de dados e trava o barramento**, levando o barômetro junto. É por isso que a rotina de recuperação de barramento é obrigatória e não opcional, e por que o barômetro é sondado primeiro.

**O que a configuração de GPS resolve que nenhum filtro resolve.** O documento de hazards manda "reaplicar qualquer configuração UBX" na volta de um reset do receptor — pressupondo que exista uma configuração. Este PRD é a primeira vez que ela é especificada. Sem essa ligação, o receptor faz brown-out no boost, volta em *Portable* silenciosamente, e a equipe perde o fix exatamente como se nunca tivesse configurado nada. A bateria de backup do módulo tem, por isso, dupla função: warm start **e** preservação da configuração.

**Por que o pacote tem exatamente 20 bytes.** Airtime LoRa é quantizada em blocos de símbolos, então há um degrau de custo entre 21 e 22 bytes de payload. Um pacote de 26 B — o primeiro rascunho — pulava um degrau e furava o teto de ciclo de trabalho já dimensionado nos docs de hardware. Cortar para 20 B preservou intactas a tabela de airtime e o orçamento de bateria. A maior economia veio de perceber que **o CRC de aplicação é redundante no rádio**: o LoRa já tem CRC de hardware. No cartão ele fica, porque ali não há camada de enlace.

**Por que a defasagem entre os rádios tem duas justificativas.** Ela nasceu de contenção de task, mas o documento de hazards observa que os dois reguladores compartilham o nó da bateria — sobrepor as duas transmissões soma carga num pacote com resistência interna alta. Uma decisão, dois motivos independentes; por isso o escalonador **verifica** a invariante em vez de presumi-la.

**Fonte única de verdade para firmware e hardware.** O mapa de pinos vem do netlist exportado. Se a placa for revisada, ele muda em exatamente um lugar do firmware.
