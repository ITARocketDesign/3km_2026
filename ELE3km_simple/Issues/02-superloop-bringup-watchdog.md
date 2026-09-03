# 02 — Superloop de 50 Hz + bring-up da placa + watchdog

**Tipo:** Bancada
**User stories:** fundação da arquitetura (superloop, barra de sobrevivência)

## What to build

O esqueleto do firmware de voo: um único laço a 50 Hz, com a ordem de boot segura e
o watchdog, mas ainda **sem lógica de sensor no ar**. É a espinha em que as fatias
seguintes penduram sensores, cartão e rádio.

`src/main.cpp`:

- **Ordem de boot obrigatória (segurança de hardware, de `DISCIPLINE.md`):** GPIO18
  HIGH, GPIO23 HIGH, GPIO32 HIGH — **todos antes de `SPI.begin()`**. WiFi e Bluetooth
  desabilitados explicitamente. Nenhum código de LED em lugar nenhum (o pino do LED é
  habilitador de TX).
- **Init da HAL** copiada do ELE3km (só instanciar e `begin()`, tratar ausência sem
  travar): I²C + BMP280 + MPU6050, UART do GPS, SX1276, microSD. Cada `begin()` que
  falha marca o módulo como ausente e o boot segue — nenhum módulo ausente trava o
  boot.
- **Superloop a 50 Hz** com `vTaskDelayUntil`/temporização por `millis()` (Δt = 20 ms).
  Por enquanto o corpo só: alimenta o watchdog, drena a UART do GPS, e imprime no
  Serial um diagnóstico dos sensores a alguns hertz (padrão copiável do `main.cpp` do
  ELE3km).
- **Watchdog (TWDT)**: as duas subscrições do ELE3km viram uma (superloop). Alimenta a
  cada volta; um deadlock real vira reset em ~5 s.

Copiar do ELE3km, verbatim, para `src/hal/`: `board.*`, `i2c_bus.*`, `bmp280.*`,
`mpu6050.*`, `gps_neo6m.*`, `radio_sx1276.*`, `sd_log.*`, `boot_counter.*`. Ajustar
só o que referenciar features removidas (ex.: qualquer resquício de `may_start_write`
ou de dois rádios); **não** reescrever driver.

## Acceptance criteria

- [ ] Ordem de boot GPIO18/23/32 HIGH antes de `SPI.begin()`, verificável no código
- [ ] WiFi e Bluetooth desabilitados no boot
- [ ] Nenhuma referência a LED em nenhum arquivo (grep)
- [ ] HAL copiada do ELE3km em `src/hal/`, sem `#include` para fora de `ELE3km_simple/`
- [ ] Um módulo ausente no boot (baro, IMU, GPS, SD, rádio) não trava o `setup()`
- [ ] Superloop temporizado a 50 Hz (Δt = 20 ms), com folga medida no Serial
- [ ] TWDT inscrito e alimentado a cada volta
- [ ] `pio run -e heltec_wifi_lora_32_V2` compila e a placa sobe, imprime diagnóstico
      de sensores no Serial e não reseta sozinha
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 01 (esqueleto, HAL a copiar, pins/DISCIPLINE)
