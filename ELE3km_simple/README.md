# ELE3km_simple

Firmware de **último recurso** para o computador de bordo ELE3km. Voa só se algo
der errado com o build principal (`../ELE3km`) perto do lançamento. Mesmo hardware
(Heltec WiFi LoRa 32 V2, rev. 1), mesma estação de solo **sem alteração**
(`../3km913hzReceiver`), mesma ferramenta de replay do cartão.

**A ideia:** onde o ELE3km escolhe qualidade ao custo de estado, este escolhe
menos estado ao custo de qualidade. As três missões continuam — gravar o voo,
transmitir ao vivo, achar o foguete depois do pouso — mas com muito menos lugares
onde o firmware pode travar numa transição de estado.

O que sai: máquina de fases (rampa/voo/pousado), detecção de liftoff e de pouso,
filtro de Kalman, fonte de posição, duas tasks FreeRTOS. Um **superloop único a
50 Hz** no lugar. Altitude é **pressão crua ao chão** (pressão-altitude contra um
datum ISA fixo); posição é **GPS bruto**; velocidade vertical é **diferença finita**.

O que fica: config do GPS (reaplicada burramente a cada 10 s), recuperação mínima
do I²C, beacon de sobrevivência por boot-loop, watchdog, e log durável pré-alocado.

- **PRD:** `Docs/ELE3km_simple_PRD.md`
- **Cursor de execução:** `Issues/NEXT.md`
- **Compatibilidade byte-a-byte:** pacote (magic `0xE`, v1, 20 B/12 B) e registro de
  log (64 B) são copiados do ELE3km. O beacon de sobrevivência usa **SF12** — o
  receptor precisa ser trocado para SF12 para ouvi-lo (recompile de uma linha).

Projeto **autônomo**: copia o código testado e os contratos congelados do ELE3km,
não compartilha nenhum arquivo. Uma mudança no ELE3km não pode quebrar este build.

## Layout do código

Dois ambientes PlatformIO, como no ELE3km. O núcleo em `src/core` é compilado
pelos dois; a HAL e o `main.cpp` só entram no build do target.

- **`src/core/`** — núcleo puro, testável no host: sem header de Arduino, sem
  relógio global, sem estado global (o tempo entra por parâmetro). Contratos
  congelados copiados do ELE3km (`telemetry_codec`, `log_codec`, `altitude`,
  `nmea`, `health`, `types`) mais o `survival_computer` — o orquestrador enxuto do
  fallback, que monta o registro (todo ciclo) e o pacote (a 1 Hz) a partir de uma
  amostra e do tempo.
- **`src/hal/`** — drivers que tocam hardware, copiados do ELE3km: `board` (ordem
  de boot segura), `i2c_bus`, `bmp280`, `mpu6050`, `gps_neo6m`, `radio_sx1276`,
  `sd_log`, `boot_counter`. Compilam só no target. Quase todos são verbatim; o
  `gps_neo6m` foi adaptado à barra (issue 05) — reaplica a config UBX a cada ~10 s
  incondicionalmente, sem os detectores de reset nem o save na memória de backup.
- **`src/main.cpp`** — o superloop de 50 Hz: bring-up da placa, watchdog, e a cola
  entre a HAL e o núcleo. Uma thread só, sem regra de arbitragem de barramento.
- **`test/`** — suítes nativas (Unity): `test_contract` (os codecs batem byte-a-byte
  com o receptor e o replay), `test_survival_computer` (a lógica do núcleo) e
  `test_log` (as invariantes do log no cartão: 8 registros de 64 B num bloco de
  512 B sem cruzar fronteira, ida e volta do cabeçalho, varredura por contador de
  boot).

## Compilar e testar

PlatformIO vive no venv do usuário, fora do PATH. Da raiz deste projeto:

```bash
# Suíte do núcleo puro (roda no notebook, sem Arduino)
~/.platformio/penv/bin/pio test -e native
```

```bash
# Compila o firmware do target (Heltec WiFi LoRa 32 V2)
~/.platformio/penv/bin/pio run -e heltec_wifi_lora_32_V2
```

As cinco greps de `DISCIPLINE.md` (segurança de hardware) têm que sair vazias — o
bloco no fim daquele arquivo é a verificação mecânica. A HAL e o `main.cpp` são
verificados na bancada; só o núcleo puro roda no host.

O estado da fila de implementação vive em `Issues/NEXT.md` — o cursor `▶` marca a
próxima issue, e cada issue feita ganha uma seção **Estado da implementação**.
