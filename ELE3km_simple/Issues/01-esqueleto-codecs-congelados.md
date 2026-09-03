# 01 — Esqueleto autônomo + codecs congelados copiados + teste de compatibilidade

**Tipo:** AFK
**User stories:** 15, 19, 23, 24, 26 (contratos), + fundação do projeto

## What to build

A fundação do fallback e a prova de que os contratos congelados foram copiados
intactos. Nenhum hardware nesta fatia — é puro núcleo, testável no host.

`ELE3km_simple/` vira um projeto **PlatformIO autônomo**: env `native` (teste no
host) e env de target (Heltec WiFi LoRa 32 V2). **Nenhum arquivo é compartilhado
com o ELE3km** — tudo que vem de lá é **copiado**, não incluído por caminho
relativo nem por symlink. Uma mudança no ELE3km nunca pode quebrar este build.

Copiar do ELE3km, verbatim, para `src/core/`:

- `telemetry_codec.*` — o formato do pacote de rádio (20 B / 12 B, magic `0xE`,
  versão `0x1`). É o que garante que o `3km913hzReceiver` decodifica sem mudança.
- `log_codec.*` (e o CRC-16 que ele usa) — o registro de 64 B que a ferramenta de
  replay lê.
- `altitude.*` — a fórmula pressão → altitude.
- `nmea.*` — o parser NMEA já testado.
- `health.h` — as constantes de bit do bitmap de saúde.
- O subconjunto de `types.h` que os itens acima usam (`SensorSample`, `LogRecord`,
  `TelemetryPacket`, `FlightPhase`, enums de fonte de posição). Copie o arquivo
  inteiro se for mais simples; não o edite para "limpar".

Copiar `DISCIPLINE.md` e `pins.h` do ELE3km verbatim (mesma placa, mesmo mapa de
pinos, mesmas regras de segurança de hardware).

Contrato duro do `core/`, herdado do ELE3km e válido para todas as issues
seguintes: **nenhum header de Arduino, nenhuma leitura de relógio global, nenhuma
variável global.** O tempo entra como parâmetro.

## Acceptance criteria

- [ ] `ELE3km_simple/platformio.ini` com env `native` e env `heltec_wifi_lora_32_V2`
- [ ] `src/core/` contém `telemetry_codec`, `log_codec` (+ CRC), `altitude`, `nmea`,
      `health.h`, `types.h` copiados do ELE3km, **sem edição de conteúdo**
- [ ] `DISCIPLINE.md` e `include/pins.h` copiados do ELE3km
- [ ] Nenhum `#include` aponta para fora de `ELE3km_simple/` (verificável por grep)
- [ ] Teste nativo: ida e volta do codec só-altitude; o tamanho codificado é
      **exatamente 12 B**
- [ ] Teste nativo: ida e volta do codec completo; o tamanho é **exatamente 20 B**
- [ ] Teste nativo: byte 0 do pacote é magic `0xE` + versão `0x1`
- [ ] Teste nativo: o registro de log codifica em **exatamente 64 B** e o CRC-16
      fecha na ida e volta
- [ ] Teste nativo: o `core/` compila e roda sem nenhum header de Arduino
- [ ] `pio test -e native` passa
- [ ] `pio run -e heltec_wifi_lora_32_V2` compila (mesmo sem `main.cpp` de voo ainda,
      um `main.cpp` mínimo que só entra em `setup()`/`loop()` vazios serve)
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- Nenhuma — pode começar imediatamente

## Estado da implementação (2026-09-02) — ✅

Projeto autônomo criado e os contratos congelados copiados verbatim.

**Feito:**
- `platformio.ini` com env `native` (Unity) e `heltec_wifi_lora_32_V2` (arduino).
  RadioLib/SdFat ficam para a issue 02, com a HAL.
- Copiados **byte-a-byte** do ELE3km para `src/core/`: `types.h`, `health.h`,
  `altitude.{h,cpp}`, `nmea.{h,cpp}`, `telemetry_codec.{h,cpp}`,
  `log_codec.{h,cpp}` (o CRC-16 vem dentro do `log_codec.cpp`). `include/pins.h` e
  `DISCIPLINE.md` também copiados verbatim.
- Nenhum `#include` aponta para fora de `ELE3km_simple/` (verificado por grep: sem
  `../` e sem caminho absoluto; todo `core/*.h` referenciado existe no conjunto).
- `src/main.cpp` mínimo (setup/loop vazios) só para o target linkar.
- Suíte `test/test_contract`: 5 casos — só-altitude é 12 B com magic `0xE`/versão
  `0x1`; ida e volta do só-altitude; completo é 20 B e ida e volta; registro de log
  é 64 B com magic `0xA5` e CRC fechando na ida e volta; **CRC copiado realmente
  valida** (um byte corrompido é rejeitado).

**Definition of Done:**
- `pio test -e native` → 5/5 passam.
- `pio run -e heltec_wifi_lora_32_V2` → SUCCESS (Flash 7,0%, RAM 6,4%).
- As cinco greps de `DISCIPLINE.md` saem vazias.

**Nada deixado de fora** do escopo da issue. A HAL, o superloop e o watchdog são
a issue 02.
