# 10 — Máquina de saúde e degradação por subsistema

**Tipo:** AFK
**User stories:** 16, 28, 72, 73, 74, 77, 79, 80, 81, 82

## What to build

A máquina de estados de saúde uniforme sobre todos os drivers, e as regras de degradação que garantem que uma falha isolada de módulo nunca derrube o sistema.

**Três estados, iguais em todo driver:** `{OK, DEGRADED, FAILED}`. Toda operação com timeout duro — sem exceção.

**Retry de período fixo, 5 s.** Um módulo em `FAILED` é retentado a cada 5 s, indefinidamente: uma falha transitória no lift-off não pode custar o sensor pelo voo inteiro. Backoff exponencial com teto de 5 s foi considerado e descartado — chegaria ao teto em três tentativas num voo de ~200 s, ou seja, a mesma coisa com mais estado para testar.

**Reverificação periódica dos registradores de configuração**, não só dos de dados. Este é o ponto menos óbvio da issue: um sensor que fez brown-out e voltou **responde normalmente e devolve dados plausíveis com a escala errada**. Ler o registrador de dados não detecta isso. Só reler a configuração detecta.

**Contador de reinicializações por sensor, gravado no log.** Se ele subir durante o voo, é sinal de que o rail está afundando e a rev. 2 precisa de um buck separado.

**Regras de degradação:**

| Falha | Comportamento |
|---|---|
| IMU ausente ou morta | Desabilita o fallback inercial; continua em GPS + barômetro |
| GPS sem fix | Continua gravando e transmitindo altitude |
| Barômetro ausente | Cai para altitude de GPS, sinalizado no pacote |
| microSD ausente ou falho | **Continua transmitindo** — uma falha de log nunca cala o link de recuperação |

**Detecção no startup:** cada subsistema é sondado e o resultado registrado. O checklist de auto-teste de startup do documento de conexões é o roteiro.

**Estado de saúde de cada subsistema em todo registro e em todo pacote** — o bitmap de 6 bits {imu, baro, gps, sd, e22, sx1276} no byte 18. É assim que a equipe diagnostica do chão o que falhou a bordo.

## Acceptance criteria

- [ ] Todo driver da `hal/` expõe `{OK, DEGRADED, FAILED}`
- [ ] Toda operação de driver tem timeout duro
- [ ] Módulo em `FAILED` retentado a cada 5 s, período fixo, indefinidamente
- [ ] Reverificação periódica dos registradores de **configuração** no caminho saudável
- [ ] Contador de reinicializações por sensor, gravado no registro de log
- [ ] Detecção de todos os subsistemas no startup, com resultado registrado
- [ ] Bitmap de saúde de 6 bits presente em todo registro e em todo pacote
- [ ] IMU ausente → fallback inercial desabilitado, GPS + barômetro continuam
- [ ] GPS sem fix → gravação e transmissão de altitude continuam
- [ ] Barômetro ausente → altitude de GPS, sinalizado no pacote
- [ ] microSD ausente ou falho → transmissão continua
- [ ] Teste nativo: com cada subsistema marcado como ausente, um por vez, o sistema continua emitindo pacotes com altitude e o bitmap reflete a falha
- [ ] Teste nativo: um sensor que volta com configuração de fábrica é detectado pela reverificação de registradores, não pelos dados
- [ ] Teste nativo: módulo em `FAILED` é retentado exatamente a cada 5 s
- [ ] No target: o checklist de auto-teste de startup do documento de conexões passa

## Blocked by

- Issue 04 (log — os contadores de reinit e o bitmap vão no registro)
- Issue 09 (recuperação de barramento I²C — sem ela o retry de 5 s falha para sempre)

## Estado da implementação

**Fechada em 2026-08-19 (fatia de núcleo).** Esta passagem por `/tdd` entrega a
parte pura e testável da issue — a máquina de saúde e o bitmap completo — e deixa a
integração de driver (que toca hardware e a cadência do barramento da issue 09)
para a fila de bancada. `pio test -e native` passa com **90 casos** (81 antes; +9
novos), `pio run -e heltec_wifi_lora_32_V2` compila (Flash 29,0 %), e os cinco
greps de `DISCIPLINE.md` saem vazios. Nenhum arquivo da HAL mudou de interface;
`log_codec.h` e `LogRecord` ficam **intocados** — o formato do cartão segue
congelado.

**Onde as coisas ficaram**

| Arquivo | O que é |
|---|---|
| `src/core/health.{h,cpp}` | **Novo.** `SubsystemHealth`: máquina `{OK, DEGRADED, FAILED}`, retentativa de período fixo de 5 s, reverificação periódica de configuração e contador de reinicializações por sensor. Puro — tempo como parâmetro, sem I/O, sem estado global. É a POLÍTICA; o chamador faz os `begin()`/`read()` reais e reporta o resultado |
| `src/core/types.h` | **Novo** `IoSubsystemHealth {sd, e22, sx1276}` — a saúde que a task io possui e que o núcleo não vê pela amostra |
| `src/core/flight_computer.{h,cpp}` | `update()` ganha `io_health` (default tudo-falso, para não mexer nos testes existentes); compõe o **bitmap de 6 bits** `{imu, baro, gps, sd, e22, sx1276}` e o grava **no pacote E no registro de log** (`result.log.health` não era preenchido antes) |
| `src/main.cpp` | A task io publica a saúde de cartão/rádios num atômico SPSC; a task flight a lê e a passa a `update()`. imu/baro/gps continuam entrando pela amostra |
| `test/test_health/` | **Novo.** 7 casos: detecção de startup, retry exato a 5 s, recuperação → OK + contador de reinit, config errada detectada pela reverificação (não pelos dados), reverificação periódica, reinit do módulo DEGRADED |
| `test/test_core/` | +2 casos: bitmap dos seis subsistemas no pacote e no registro; cada subsistema ausente, um por vez, mantém o pacote no ar e apaga só o seu bit |

**Duas decisões, autorizadas antes de codar**

*Contador de reinit por sensor: runtime + bitmap, fora do registro.* O registro de
64 B está cheio (bytes 0–61 atribuídos, 62–63 CRC) e o formato é congelado — crescer
o registro quebraria o invariante de 8 registros por bloco de 512 B, e versioná-lo
respingaria no harness de replay da issue 14. O contador vive na máquina de saúde
(`reinit_count()`), é exposto em runtime, e a falha aparece no bitmap. A gravação
durável do contador no cartão fica para uma decisão futura de mudança de formato.

*Escopo de núcleo; o refactor de interface dos drivers fica na bancada.* A issue
pede que TODO driver da `hal/` exponha `{OK, DEGRADED, FAILED}` e reveja os
registradores de configuração. Isso é mudança de interface em cada adaptador
(um `verify_config()` que relê os registradores de configuração reais) e só se
verifica no target. Aqui mora a máquina que esses drivers vão usar, testada por
comportamento externo; a adoção por driver é da fila de bancada.

**Critérios: onde cada um ficou**

- *Máquina `{OK, DEGRADED, FAILED}` uniforme* — entregue em `core/health.h`,
  testada. Adoção por cada driver da `hal/`: bancada.
- *Retry de FAILED a cada 5 s, período fixo* — a POLÍTICA está na máquina e testada
  ao milissegundo (`test_failed_module_retried_exactly_every_5s`). O `flight_task`
  ainda recupera o barramento na cadência da issue 09 (recuperação imediata na
  falha); trocar essa cadência pela de 5 s toca a temporização física do H5 e fica
  na bancada, junto com a adoção por driver.
- *Reverificação periódica dos registradores de configuração* — a política e a
  detecção ("volta de fábrica é pega pela config, não pelos dados") estão testadas
  em `core/health.h`. O `verify_config()` que relê os registradores reais é o
  pedaço de driver, da bancada.
- *Contador de reinit por sensor, no registro de log* — ver decisão acima: runtime +
  bitmap agora; carimbo no registro adiado.
- *Bitmap de 6 bits em todo registro e todo pacote* — **feito e testado**, inclusive
  o preenchimento de `result.log.health`, que não existia.
- *Detecção de todos os subsistemas no startup, com resultado registrado* — o probe
  de startup já existe em `setup()` (Serial); `begin(present, now)` da máquina é o
  ponto de entrada dessa detecção quando os drivers a adotarem.
- *Regras de degradação (IMU/GPS/baro/microSD)* — o comportamento de sistema já vinha
  de fatias anteriores; o teste `test_each_missing_subsystem...` fixa que cada
  ausência mantém o pacote com altitude no ar e reflete a falha no bitmap.
- *No target: checklist de auto-teste de startup* — fila de bancada, como previsto.

**Fila de bancada que esta fatia acrescenta**

Adoção da `SubsystemHealth` por driver: `verify_config()` relendo os registradores
de configuração do BMP280 e do MPU6050; a troca da cadência de recuperação I²C da
imediata (issue 09) para a de 5 s da máquina; e o carimbo em runtime dos contadores
de reinit no Serial. Tudo verificável só com placa + sensores na bancada.
