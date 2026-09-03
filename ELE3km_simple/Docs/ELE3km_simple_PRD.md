# PRD — ELE3km_simple (firmware de último recurso)

**Projeto:** ELE3km_simple — versão de sobrevivência do computador de bordo ELE3km
**Alvo:** Heltec WiFi LoRa 32 **V2** (ESP32), placa rev. 1, a mesma do ELE3km
**Rádio:** SX1276 onboard (915–928 MHz, 100 mW) — único caminho
**Build:** PlatformIO, projeto **autônomo** (não compartilha arquivos com o ELE3km)
**Status:** Pronto para implementação

---

## Relação com o ELE3km

Este firmware **não substitui** o ELE3km. Ele é um **fallback de último recurso**:
voa só se algo der errado com o build principal perto do lançamento e não houver
tempo de depurar. Roda no mesmo hardware, fala com a **mesma estação de solo sem
nenhuma alteração** (`3km913hzReceiver`), e grava um cartão que a **mesma
ferramenta de replay** (ELE3km issue 14) lê sem alteração.

A relação de projeto é uma só: **onde o ELE3km escolheu qualidade ao custo de
estado, o ELE3km_simple escolhe menos estado ao custo de qualidade.** Ele executa
as mesmas três missões, com muito menos lugares onde pode travar numa transição de
estado.

O ELE3km_simple **copia** do ELE3km o código já testado e os contratos congelados
(codec de pacote, codec de log, altitude, NMEA, drivers da HAL) e **reimplementa**
só a cola, que é trivial. Nenhum arquivo é compartilhado: uma mudança no ELE3km
nunca pode quebrar o fallback. É o ponto inteiro de um build de reserva.

## Problem Statement

O ELE3km é rico e correto, mas a robustez dele vem de máquinas de estado: fases de
voo (rampa → voo → pousado) com detecção de liftoff e de pouso, reuso de
referência barométrica na volta de um reset, um filtro de dois canais com reset por
divergência, uma máquina de fonte de posição que troca entre GPS, ponte inercial e
última válida, e duas tasks FreeRTOS com um ring buffer entre elas. Cada uma dessas
máquinas melhora os dados **e** é um lugar onde o firmware pode entrar no estado
errado: um liftoff perdido congela a referência errada; um pouso falso no apogeu
joga fora a descida inteira; um reuso de referência ruim voa com um zero velho.

Perto do lançamento, com o foguete na rampa, a equipe pode não ter como diagnosticar
uma dessas máquinas. Precisa de um firmware que faça as mesmas três coisas —
**gravar o voo, transmitir ao vivo, e ser achável depois do pouso** — sem nenhuma
máquina de estado de voo que possa estar no estado errado.

## A barra de sobrevivência

O ELE3km_simple garante, com o menor número possível de modos de falha:

1. **Todo dado bruto de sensor vai ao cartão** num registro de tamanho fixo,
   recuperável por varredura (magic + sequência + contador de boot + CRC).
2. **Um pacote com altitude e posição GPS bruta sai a ~1 Hz**, do power-on até a
   bateria acabar, sem depender de detectar liftoff nem pouso.
3. **A altitude sempre chega**, venha ou não a posição: um pacote só-altitude cobre
   os vãos de GPS. Nunca há silêncio total enquanto houver energia e rádio.

Fora dessa barra, os dados são mais crus e a telemetria é mais burra — de propósito.

## O que é deliberadamente removido, e por quê

| Removido do ELE3km | Por quê é seguro remover sob a barra de sobrevivência |
|---|---|
| **Máquina de fases (rampa/voo/pousado)**, detecção de liftoff e de pouso | A fase existia para (a) congelar a referência de altitude, (b) baixar a taxa de log pós-pouso, (c) carimbar os bits de fase. Nenhum é carga sob a barra. Sem detecção, não há transição para errar. |
| **Referência barométrica de solo e reuso na volta do reset (NVS)** | Substituídos por **pressão crua ao chão**: a altitude do pacote é pressão-altitude contra um **datum ISA fixo (101325 Pa)**, e o operador subtrai a leitura da rampa no chão. Sem referência a bordo, sem estado, sem problema de reset em voo. |
| **Filtro de Kalman de dois canais + reset por divergência** | Velocidade vertical vira **diferença finita** da pressão-altitude. Nenhum estado de filtro para divergir. |
| **Máquina de fonte de posição (GPS ↔ inercial ↔ última válida)** | Posição é **GPS bruto**: fix válido → pacote completo (fonte = GPS); sem fix → pacote só-altitude (fonte = nenhuma). Sem ponte inercial, sem replay de última válida. |
| **Duas tasks FreeRTOS + ring buffer + atômicos** | **Superloop único.** Some o ring, os atômicos e — de graça — a regra de arbitragem do barramento SPI (código sequencial nunca tem dois mestres SPI ao mesmo tempo). |
| **Detecção de reset do GPS (restart de NMEA, sats a zero após TX) + modo Stationary no pouso** | A **config UBX é reenviada incondicionalmente a cada ~10 s.** Um receptor que resetou é reconfigurado em ≤10 s, sem lógica de detecção. |
| **Contador de reinit por sensor, retentativa periódica de 5 s, watermarks de stack, carimbos de diagnóstico** | Diagnóstico, não sobrevivência. Os campos permanecem no registro de log (gravados como zero) para o formato não mudar. |

## O que é mantido (robustez que sobrevive à simplificação)

- **Config do GPS (Airborne <4g, 5 Hz, sentenças inúteis desligadas, buffer UART
  512 B)**, reaplicada burramente a cada ~10 s. É a única remoção que custaria a
  missão: sem ela o NEO-6M rejeita a própria solução no boost. (Mesmo configurado,
  o pico de boost >4 g perde a solução; a config mantém o fix nas fases sub-4 g e
  acelera a reaquisição.)
- **Recuperação mínima do barramento I²C**: no read que falha, clock-out por
  bit-bang + `begin()` de novo, no máximo um módulo por ciclo. Guarda o payload
  prioritário (altitude vem do baro por I²C).
- **Beacon de sobrevivência por boot-loop**: contador de boot na memória RTC; acima
  de um limiar numa janela curta, entra num caminho **só-rádio** (inicializa só o
  SX1276, pula SD/GPS/sensores) e emite um beacon lento em SF12.
- **Watchdog (TWDT)** alimentado a cada volta, e um **watchdog de cadência de TX**:
  se nenhum pacote chegou ao rádio em N s, força um reset — um travamento vira
  reboot-para-beacon rápido em vez de silêncio.
- **Log durável**: arquivo **pré-alocado contíguo** + escrita em blocos, arquivo
  novo por boot, contador de boot em cada registro.
- **Regras de disciplina de hardware** (`DISCIPLINE.md`): são segurança de
  hardware, não complexidade, e continuam valendo.

## Arquitetura — superloop de 50 Hz

Um único laço, um único caminho do boot à morte da bateria:

```
setup(): board/HAL init → GPS UBX blast → SD pré-aloca arquivo → radio SF7
         (se boot-loop detectado: caminho só-rádio SF12 e nada mais)

loop() a 50 Hz (Δt = 20 ms):
  1. alimenta o watchdog
  2. drena a UART do GPS; a cada ~10 s reenvia a config UBX
  3. lê IMU (50 Hz) e baro (25 Hz, subciclo)
  4. em falha de read: recuperação I²C (≤1/ciclo)
  5. calcula pressão-altitude (datum fixo) e velocidade vertical (diferença finita)
  6. monta o registro de 64 B e grava no cartão (bloco, sequencial)
  7. a cada 1 s (a cada 50 voltas): monta o pacote, incrementa a sequência,
     transmite pelo SX1276 (SF7). Fix válido → 20 B; sem fix → 12 B.
```

Uma travada de cartão de 100–250 ms atrasa algumas amostras e no máximo empurra um
pacote de 1 Hz — invisível sob a barra. Uma travada **dura** (SD+rádio dividem a
mesma VSPI na rev. 1, então nenhum firmware transmite durante uma escrita travada)
é pega pelo watchdog → reboot → beacon. A pré-alocação + escrita em blocos deixam a
travada rara; o watchdog a deixa barata.

## Contratos reusados byte-a-byte

- **Pacote de rádio** (`PACKET_FORMAT.md` do ELE3km): magic `0xE`, versão `0x1`,
  20 B completo / 12 B só-altitude, little-endian. **O `3km913hzReceiver` decodifica
  sem nenhuma mudança.** Campos preenchidos assim:
  - **altitude (i16, m):** pressão-altitude contra o datum ISA de 101325 Pa.
  - **velocidade vertical (i16, dm/s):** diferença finita da pressão-altitude.
  - **bits de fase (flags 0–2): fixos em `1` (em voo)** — assim a estação lê os
    bits 5–7 como qualidade de fix o tempo todo (nunca como amostras pós-pouso).
  - **fonte de posição (flags 3–4):** `1` (GPS) com fix, `0` (nenhuma) sem fix.
  - **qualidade de fix (flags 5–7):** indicador GGA, 0–7.
  - **saúde (byte 18):** bitmap honesto — IMU, baro, GPS-vivo, microSD, SX1276; bit
    4 (E22) reservado 0; bit 6 = altitude é barométrica (1) ou caiu para GPS por
    baro ausente (0).
  - **tempo (u16, ds):** decissegundos **desde o boot** (não há liftoff); satura em
    ~109 min; depois a estação ordena por sequência, como já documentado.
  - **sequência (u16):** global, incrementa por pacote.
- **Registro de log** (`log_codec.h` do ELE3km): 64 B, 8 por bloco de 512 B, magic +
  contador de boot + sequência + CRC-16. Campos brutos reais; campos de features
  removidas gravados como zero (padrão já sancionado no formato). Campo de
  referência barométrica no cabeçalho = o datum fixo, para a análise saber o zero.

## Testes e disciplina

- **Núcleo puro testável no host** (env `native`), como no ELE3km: os codecs
  copiados, a pressão-altitude, a velocidade por diferença finita e a regra de porta
  de fix (sats ≥ 4, HDOP ok) são testados nativamente. Um teste dedicado afirma que
  os bytes do pacote batem com o formato congelado (20/12 B, magic/versão) e que o
  registro é 64 B com CRC — a garantia de compatibilidade com o receptor e o replay.
- **HAL e `main.cpp`** compilam só no env alvo e são verificados na bancada.
- **As cinco greps de `DISCIPLINE.md`** (copiadas) têm que sair vazias: sem código
  de LED, sem escrita direta nos pinos 12/25 do RF-switch, sem domínio analógico,
  `src/core/` sem headers Arduino, sem número de GPIO literal fora de `pins.h`.

## User stories do PRD v3 — cobertura

**Cobertas (com dados mais crus):** 1, 2, 3 (um só rádio — ver não-metas), 8, 9,
11, 13, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 29, 34, 35, 36, 37*, 45, 46, 50.

**Conscientemente NÃO-metas** (removidas com a máquina que as servia): 4, 6, 7
(detecção de pouso e média pós-pouso — não há pouso); 10, 12 (fonte de posição
inercial); 17 (fase real no pacote — fica fixa); 21, 32, 33 (saídas do filtro);
31 (taxa de log cai no pouso — taxa é fixa); 39–44, 47–49 (filtro e fusão); 28
(contador de reinit por sensor); 52–55 (referência de solo e persistência).

**Fora de escopo por hardware, herdado do ELE3km:** 3 e 14 (dois rádios — o E22 foi
abandonado; ver `Docs/ELE3km_drop_e22_single_radio.md`); 5 e 18 (SF12/defasagem de
dois rádios). O beacon de sobrevivência usa SF12 num rádio só.

\* 37: a config é reenviada a cada 10 s em vez de salva em memória com bateria de
backup; o efeito de sobrevivência (voltar configurado depois de um reset) é o mesmo,
com menos estado.

## Decisões de projeto (registro do grill de 2026-09-02)

1. **Barra de sobrevivência**, não paridade de qualidade. A simplificação é no
   *como*, não no *quê*: as três missões permanecem.
2. **Máquina de fases removida por inteiro**, pressão crua ao chão.
3. **Pacote byte-a-byte idêntico** — o operador não reflasheia a estação de solo
   para trocar para o fallback.
4. **Superloop, protegido por watchdog** — a regra de arbitragem SPI deixa de
   existir. Na rev. 1, SD e rádio dividem a VSPI, então a proteção contra travada
   dura é reboot, não isolamento (o ELE3km também depende disso).
5. **Filtro e fonte de posição removidos** — GPS bruto + velocidade por diferença
   finita.
6. **Config do GPS mantida, mas burra** — reenvio incondicional a cada 10 s.
7. **Recuperação I²C mínima mantida.**
8. **Beacon de boot-loop mínimo mantido.**
9. **Registro de log de 64 B reusado verbatim.**
10. **Projeto autônomo, código copiado, sem arquivos compartilhados.**
11. **Laço a 50 Hz**, log todo ciclo, sem troca de taxa.
