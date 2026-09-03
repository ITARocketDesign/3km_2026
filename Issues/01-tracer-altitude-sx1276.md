# 01 — Tracer bullet: altitude no ar pelo SX1276

**Tipo:** AFK
**User stories:** 2, 11, 15, 46, 64, 67, 98, 100, 104, 105, 106, 107

## What to build

A primeira fatia vertical completa: a placa liga, lê pressão, deriva altitude e um receptor em 915 MHz ouve um pacote a 1 Hz. Corta todas as camadas — build, mapa de pinos, HAL, núcleo puro, codec, rádio — no caminho mais fino possível.

O SX1276 é o primeiro rádio deliberadamente: vive no rail de 5 V do regulador interno do Heltec, transmite 100 mW, e não carrega o risco de destruir o PA de 1 W do E22 numa bancada sem antena. O E22 entra na issue 03.

Escopo do núcleo nesta fatia: `FlightComputer::update(SensorSample, t) → { LogRecord, lista de TelemetryPacket }` com a altitude vindo direto da fórmula barométrica, sem filtro. O estimador entra na issue 07 e substitui esse caminho direto. O campo `t` do pacote é relativo ao boot nesta fatia; passa a ser relativo ao liftoff na issue 06.

Contrato duro do `core/`, estabelecido aqui e válido para todas as issues seguintes: **nenhum header de Arduino, nenhuma leitura de relógio global, nenhuma variável global.** O tempo entra como parâmetro em toda API.

O pacote só-altitude tem **12 B exatos**. O pacote completo (20 B) entra na issue 02, mas o layout dos dois é fixado e documentado agora, little-endian, porque a estação de solo depende dele.

As regras de disciplina de §12 do PRD custam zero código e vão no primeiro commit, como documento no repositório e como comentários nos pontos de risco. A mais cara de descobrir tarde: **nenhum código de LED em lugar nenhum** — o pino do LED do módulo é o habilitador de transmissão, e piscar o LED liga o PA de 1 W.

## Acceptance criteria

- [ ] `platformio.ini` com ambiente `native` (framework de teste) e ambiente de target (Heltec WiFi LoRa 32 V2)
- [ ] `include/pins.h` como fonte única do mapa de pinos, batendo com o netlist de `ELE3km_connections.md`, com revisão declarada
- [ ] Documento de disciplina no repositório com as oito regras de §12 e a consequência de violar cada uma
- [ ] Nenhuma referência a LED em nenhum arquivo do projeto (verificável por grep em revisão)
- [ ] Ordem de boot obrigatória: GPIO18 HIGH, GPIO23 HIGH, GPIO32 HIGH — **todos antes de `SPI.begin()`**
- [ ] WiFi e Bluetooth desabilitados explicitamente no boot
- [ ] `hal/` com adaptador BMP280 (I²C), com timeout duro em toda operação
- [ ] `core/` com codec de telemetria: serialização do pacote só-altitude, little-endian
- [ ] `core/` com `FlightComputer::update()` devolvendo `LogRecord` e lista de `TelemetryPacket`
- [ ] `hal/` com adaptador SX1276 configurado em 915–928 MHz — **não** 868 MHz
- [ ] TX não-bloqueante com IRQ de fim de transmissão; a borda de IRQ é sempre confirmada contra o registrador de status
- [ ] Nenhuma espera indefinida no sinal de ocupado: timeout limitado mais caminho de recuperação por reset
- [ ] Teste nativo: ida e volta do codec só-altitude
- [ ] Teste nativo: o tamanho codificado do pacote só-altitude é **exatamente 12 B**
- [ ] Teste nativo: altitude aparece em todo pacote emitido
- [ ] Teste nativo: o núcleo compila e roda sem nenhum header de Arduino
- [ ] Testes passam em `pio test -e native`
- [ ] No target: a placa transmite a 1 Hz e um receptor decodifica altitude coerente com a altitude local

## Blocked by

- Nenhuma — pode começar imediatamente
