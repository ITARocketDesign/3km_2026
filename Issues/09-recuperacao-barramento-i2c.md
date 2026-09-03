# 09 — Recuperação do barramento I²C e ordem de probe

**Tipo:** AFK
**User stories:** 75, 76, 78

## What to build

A defesa contra o modo de falha mais provável desta placa. Separada da máquina de saúde genérica (issue 10) porque é um mecanismo específico, com uma causa raiz conhecida e uma consequência que custa a altitude — o payload prioritário.

**A causa raiz.** O documento de hazards observa que a IMU tem o GND flutuando e que, nesse estado, ela é alimentada parasiticamente pelos diodos de proteção através dos pull-ups do barramento. Uma peça assim **não "deixa de responder" limpo** — ela segura a linha de dados e trava o barramento, levando o barômetro junto. Um único escravo travado derruba a IMU **e** o barômetro, e com eles a altitude.

**Rotina de recuperação, obrigatória e não opcional:**

1. Soltar a linha de dados
2. Pulsar o clock ~9 vezes
3. Emitir condição de parada
4. Reinicializar o driver

Sem essa rotina, a reinicialização periódica da issue 10 falha para sempre — o barramento continua travado e nenhuma quantidade de retries resolve.

**Ordem de probe: barômetro primeiro.** Se a IMU travar o barramento durante o startup, pelo menos fica registrado que o barômetro estava saudável. A ordem inversa perde essa informação.

**Orçamento de tempo.** As tentativas de recuperação não podem comer o ciclo: timeout de barramento explícito e baixo, e **no máximo um módulo tentado por ciclo**. A recuperação leva 10–50 ms, e é exatamente essa janela que motiva o buffer UART de 512 B da issue 02.

**O barramento não pode subir para 400 kHz** — o pino de dados é também o controle de Vext do módulo. A 100 kHz o orçamento é de ~15% para a IMU e ~2% para o barômetro, ~17% total, com folga para retries e recuperação.

**Não suspender I²C durante TX.** O custo de suspender (14% das amostras de IMU perdidas, buraco sistemático no predict do Kalman) é maior que o benefício de evitar retries ocasionais por glitch de EMI do PA de 1 W. As defesas desta issue tratam a consequência independentemente da causa. Para o diagnóstico pós-voo, os timestamps de erro de I²C são correlacionáveis com os timestamps de TX do escalonador — se houver correlação, a rev. 2 precisa de blindagem ou roteamento separado.

## Acceptance criteria

- [ ] Rotina de recuperação de barramento implementada: soltar SDA, ~9 pulsos de clock, condição de parada, reinicialização do driver
- [ ] Ordem de probe no startup: **BMP280 antes do MPU6050**
- [ ] Timeout de barramento explícito e baixo em toda operação I²C
- [ ] No máximo **um** módulo tentado por ciclo
- [ ] Barramento a 100 kHz; nenhuma configuração para 400 kHz em lugar nenhum
- [ ] I²C **não** é suspenso durante transmissão
- [ ] Erros de I²C registrados com timestamp, correlacionáveis com os timestamps de TX na análise pós-voo
- [ ] Teste no target: com um escravo forçado a segurar SDA, a rotina destrava o barramento e o driver volta a operar
- [ ] Teste no target: IMU removida do barramento — o barômetro continua sendo lido e a altitude continua sendo transmitida
- [ ] Teste no target: a recuperação não estoura o ciclo de 10 ms de forma sustentada

## Blocked by

- Issue 07 (estimador — traz a HAL do MPU6050, o segundo dispositivo do barramento)

## Estado da implementação

**Fechada em 2026-08-19.** Metade dos critérios já vinha satisfeita de fatias
anteriores; o que a issue de fato acrescentou é a rotina de recuperação e a
orquestração de no-máximo-um-módulo-por-ciclo na task de voo. `pio test -e native`
passa com os mesmos 81 casos (esta fatia é HAL/target — o env `native` só compila
`src/core/`, então não há teste nativo novo, como na parte de HAL da issue 11).
`pio run -e heltec_wifi_lora_32_V2` compila (RAM 26,1 %), e os cinco greps de
`DISCIPLINE.md` saem vazios.

Nenhuma linha de `src/core/` mudou: recuperação de barramento é ação física sobre
os pinos, mora inteira na HAL.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/hal/i2c_bus.{h,cpp}` | **Novo.** `i2c_bus_recover()`: solta SDA, ~9 pulsos de SCL, STOP, `Wire.begin()` a 100 kHz. Só o mecanismo (mitigação #1 do H5) |
| `src/main.cpp` (`flight_task`) | Numa leitura I²C que falha, chama `i2c_bus_recover()` + `begin()` do dispositivo; guarda de um módulo por ciclo; IMU antes do baro |

**Critérios que já vinham satisfeitos, e de onde**

- *Ordem de probe BMP280 antes do MPU6050* — `setup()` em `main.cpp` desde a issue 07.
- *Timeout duro e baixo em toda operação* — `bus_.setTimeOut(25 ms)` no `begin()`
  dos dois adaptadores (`bmp280.cpp`, `mpu6050.cpp`).
- *100 kHz, nenhuma configuração para 400 kHz* — `I2C_HZ = 100000` em `pins.h`
  (fonte única); o único `400k` no repo é o comentário "não subir".
- *I²C não suspenso durante TX* — a `flight_task` lê os sensores a cada ciclo,
  independente do escalonador; nada condiciona I²C à disputa de rail/barramento.

**Duas decisões, autorizadas antes de codar**

*A rotina mora num módulo compartilhado, não num `recover()` por adaptador.* A
recuperação física (9 clocks + STOP) é do barramento inteiro, não de um
dispositivo; só o reinit do driver é por peça. Então o bit-bang vive uma vez em
`hal/i2c_bus`, e a `flight_task` re-roda o `begin()` do dispositivo afetado depois
— que reaplica timeout e configuração. (A alternativa espelhava o `recover()` dos
rádios em cada adaptador, duplicando o bus-clear.)

*AC7 = mecanismo agora, carimbo durável na issue 10.* A 09 entrega a recuperação e
sinaliza o erro de I²C com timestamp (`now_ms` no `Serial`, correlacionável com os
timestamps de TX do escalonador). O registro durável no cartão — o bitmap de saúde
por registro e o contador de reinit por sensor — é da issue 10, que já os lista
como critérios próprios sobre o formato de 64 B. `log_codec.h` e `LogRecord` ficam
**intocados**; o formato do cartão continua congelado.

**Gatilho e cadência: o que a 09 faz e o que a 10 herda**

O gatilho é uma leitura que retorna false (timeout do adaptador = barramento
travado, H5). A recuperação custa ~10–50 ms — um estouro de ciclo, absorvido pelo
buffer de UART de 512 B do GPS (issue 02), nunca sustentado: se o `begin()` pós
recuperação falhar, `g_imu_ok`/`g_baro_ok` latcheiam false e o módulo **para de ser
lido**, então uma peça arrancada dispara uma recuperação e depois silencia, sem
comer o ciclo voo afora.

O que **nada nesta fatia** faz é reerguer esse flag: a retentativa periódica de 5 s
de um módulo `FAILED`, a máquina `{OK, DEGRADED, FAILED}` e a reverificação dos
registradores de configuração são da issue 10 — ela chama `i2c_bus_recover()` como
primitiva sob a própria cadência.

**Fila de bancada** (as três ACs de target, fora do `/tdd`): um escravo forçado a
segurar SDA e a rotina destrava o barramento; IMU removida e o baro continua sendo
lido e a altitude no ar; a recuperação não estoura o ciclo de 10 ms de forma
sustentada. Precisam de placa + MPU6050 + um jeito de segurar SDA (jumper à mão).
