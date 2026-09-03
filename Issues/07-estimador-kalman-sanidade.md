# 07 — Estimador Kalman com sanidade numérica

**Tipo:** AFK
**User stories:** 29, 33, 39, 40, 46, 47, 48, 49, 85

## What to build

O filtro que substitui a altitude barométrica direta da issue 01 por uma estimativa fundida, e a IMU que o alimenta.

**Estimador no `core/` puro**, com dois canais:

- **Horizontal:** posição + velocidade. Predição pela aceleração da IMU, correção pelo GPS.
- **Vertical:** altitude + velocidade vertical. Predição pela aceleração da IMU, correção pelo barômetro e pela altitude do GPS.

Expõe a estimativa corrente, a covariância por canal e a fonte de posição ativa.

**A assimetria que estrutura o projeto:** o mesmo sensor inercial é excelente num eixo e inútil no outro. **Verticalmente** a integração é confiável, porque a gravidade dá referência de atitude; **horizontalmente** é lixo, porque sem magnetômetro o heading não é observável. É por isso que a altitude é o payload prioritário e a posição não é.

**O R do barômetro é função da velocidade estimada:** peso alto abaixo de ~30 m/s, quase nulo acima de ~100 m/s. A leitura barométrica é ruim em alta velocidade. Durante o boost a altitude vem essencialmente da integração vertical do acelerômetro.

**Degraus impossíveis de pressão são rejeitados por limite de taxa de variação**, em vez de alimentados ao filtro. Amostras de IMU que bateram no fundo de escala são **marcadas como saturadas** e excluídas, ou têm a covariância inflada, no passo de predição — dados de boost clipados não podem ser integrados como válidos.

**Sanidade numérica a cada passo (§15 do PRD).** Após cada predict e update, verificar `isfinite()` em todo o vetor de estado e na diagonal da covariância, e verificar limites físicos:

| Canal | Limite na diagonal | Justificativa |
|---|---|---|
| Posição horizontal | **1e8 m²** (10 km²) | Incerteza > 10 km = estimativa inútil |
| Altitude | **2,5e7 m²** (25 km²) | Altitude máxima plausível ao quadrado |
| Velocidades | **2,5e5 m²/s²** ((500 m/s)²) | Acima da velocidade máxima do veículo |

**Caminho de recuperação:** detectada divergência, o estimador reseta para a última medição válida disponível (GPS para posição, barômetro para altitude), reinicializa a covariância com incerteza máxima, e reconverge naturalmente — o mesmo caminho que percorre no boot. Um **contador de resets do estimador** é logado; se subir durante o voo, o modelo do filtro precisa de revisão.

Se o barômetro estiver ausente, a altitude cai para a do GPS e isso é sinalizado no pacote.

Tudo — filtro, sanidade e caminho de reset — vive inteiramente no `core/` e é testável no host.

## Acceptance criteria

- [ ] `hal/` com adaptador MPU6050 (I²C), com timeout duro, expondo marcação de saturação de fundo de escala
- [ ] `core/` com estimador de dois canais, puro, tempo como parâmetro
- [ ] R do barômetro variável com a velocidade estimada
- [ ] Rejeição de degraus de pressão por limite de taxa de variação
- [ ] Amostras de IMU saturadas marcadas e excluídas ou com covariância inflada no predict
- [ ] Verificação `isfinite()` no vetor de estado e na diagonal da covariância após cada predict e update
- [ ] Limites físicos na diagonal verificados nos três canais, com os valores da tabela
- [ ] Reset para a última medição válida com covariância reinicializada alta
- [ ] Contador de resets do estimador exposto e gravado no registro de log
- [ ] Barômetro ausente → altitude de GPS, sinalizado no pacote
- [ ] Teste nativo: trajetória sintética com ruído — a estimativa acompanha dentro de erro limitado
- [ ] Teste nativo: a correção de GPS reduz o erro
- [ ] Teste nativo: durante um vão de GPS o filtro roda só em predição e a incerteza de posição cresce
- [ ] Teste nativo: com velocidade alta o peso do barômetro cai — **verificado pelo comportamento da saída, não pela matriz**
- [ ] Teste nativo: alimentado com medições NaN, degraus impossíveis e ruído extremo, a divergência é detectada, o estimador reseta para a última medição válida e reconverge em poucos ciclos, e o contador incrementa
- [ ] Teste nativo: amostras no fundo de escala são marcadas e não integradas como válidas
- [ ] **Nenhum teste inspeciona a matriz de covariância diretamente** — só o comportamento externo

## Blocked by

- Issue 01 (tracer bullet — tipos, FlightComputer, seam de teste)

## Estado da implementação

Tudo implementado menos o critério de target implícito (a IMU real preenchendo o
canal inercial numa bancada), que precisa de placa. `pio test -e native` passa com
64 casos em sete suítes — 7 novos na suíte `test_estimator`. `pio run -e
heltec_wifi_lora_32_V2` compila (RAM 26,1 %), e os cinco greps de `DISCIPLINE.md`
saem vazios.

O estimador vive inteiro no `core/`, puro e com o tempo como parâmetro. Cada canal
é um filtro de Kalman cinemático de dois estados (posição, velocidade); o vertical,
o norte e o leste são três instâncias do mesmo `Channel`. Nenhum teste abre a matriz
de covariância: o peso do barômetro, o crescimento da incerteza num vão de GPS, a
exclusão da saturação e a detecção de divergência são todos verificados pela saída.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/estimator.{h,cpp}` | O estimador de dois canais, o `Channel` de Kalman 2×2, a sanidade e o reset |
| `src/core/flight_computer.{h,cpp}` | Passou a compor o `Estimator`; velocidade vertical e resets no pacote e no log |
| `src/core/types.h` | `SensorSample` ganhou `gyro_ddps`, `imu_valid` e `accel_saturated` |
| `src/hal/mpu6050.{h,cpp}` | Adaptador I²C: mg e décimos de grau/s, timeout duro, marcação de saturação |
| `src/main.cpp` | Lê a IMU a 100 Hz na task flight e alimenta a amostra |
| `include/pins.h` | `ADDR_MPU6050_ALT` (0x69) para o probe do segundo endereço |
| `test/test_estimator/` | As sete invariantes do estimador |

**Cinco decisões que divergem do texto da issue ou o completam**

*A altitude OBSERVÁVEL do pacote e do log continua sendo a barométrica da fase, não
a fundida.* O estimador é a fonte da velocidade vertical (campo do pacote que era 0
até aqui), da posição fundida, do contador de resets e do fallback de barômetro
ausente — mas não sobrescreve o valor de altitude que o solo vê. O motivo é
concreto: o `LogRecord` não tem campo de altitude fundida, e a suíte existente fixa
o valor barométrico (215,3 m, 77 m). Rerroteá-lo pela fusão quebraria testes que
passam sem ganho de contrato — a altitude é o payload prioritário e o barômetro é a
melhor fonte dela em subsônico. A fusão vertical age sobre a velocidade e é o
substrato onde a desponderação transônica opera.

*A altitude do GPS não é fundida enquanto o barômetro está presente.* Ela é MSL e a
altitude barométrica é relativa à referência de solo — dois zeros diferentes. Fundir
as duas cruas puxaria a estimativa para um ponto entre os referenciais. A altitude do
GPS só assume quando o barômetro está ausente, e aí o bit kAltRef (issue 06) já
sinaliza ao solo que o referencial mudou. A reconciliação (subtrair a altitude de GPS
do solo) é máquina para a fonte de posição da issue 08.

*A rejeição de degrau vive no domínio de ALTITUDE, não de pressão.* O filtro recebe
altitude (a pressão já virou altitude na fase), então o limite de taxa é em m/s de
altitude. É equivalente ao degrau de pressão pela monotonicidade da ISA, e é onde o
filtro trabalha.

*A divergência é detectada DEPOIS do passo, não prevenida antes.* Um degrau finito
impossível é rejeitado por taxa e não alimenta o filtro. Um NaN, porém, passa de
propósito (comparação com NaN é falsa): ele contamina o estado, a sanidade pós-passo
o pega, o canal reseta para a última medição válida e o contador sobe. É o caminho
que a acceptance pede — "a divergência é detectada… e o contador incrementa" —, e
pré-rejeitar tudo deixaria o contador em zero.

*A aceleração vertical de navegação é o eixo z de corpo menos 1 g.* Sem atitude, é
grosseira fora da rampa, mas é justamente na rampa e no boost — quando o foguete está
alinhado com o trilho e o barômetro é ruim (H12) — que a integração vertical importa.
Os três eixos brutos vão ao log ao lado, então a decisão pode ser refeita depois.

**O que as fatias seguintes herdam**

*A issue 08 (fonte de posição)* herda a posição fundida e a incerteza horizontal já
prontas, e a distinção grosseira Gps/Ins que o estimador expõe — refina para a
máquina completa com LastValid e idade, e é o lugar da reconciliação de referencial
da altitude do GPS.

*A issue 09 (recuperação de I²C)* herda o adaptador MPU6050 com timeout duro; a
rotina de recuperação de barramento e a ordem de probe são dela.

*A issue 14 (harness de replay)* destrava agora: o estimador é puro e determinístico,
então um voo gravado pode ser reprocessado e comparado no host.
