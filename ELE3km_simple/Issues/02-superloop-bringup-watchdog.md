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

## Estado da implementação (2026-09-02)

Feito. O esqueleto de voo sobe e o superloop roda; falta só a verificação de
bancada (placa física), que espera o hardware.

- **HAL copiada verbatim** para `src/hal/`: `board.*`, `i2c_bus.*`, `bmp280.*`,
  `mpu6050.*`, `gps_neo6m.*`, `radio_sx1276.*`, `sd_log.*`, `boot_counter.*`.
  Nenhum ajuste foi preciso — os arquivos do ELE3km já são de rádio único
  (SX1276) e não têm resquício de `may_start_write` nem de dois rádios. `board.cpp`
  já prende o E22 abandonado em reset (NSS alto, NRST baixo). Nenhum `#include`
  aponta para fora de `ELE3km_simple/`.
- **`platformio.ini`**: `RadioLib@^7.7.1` e `SdFat@^2.3.0` adicionados ao env
  `heltec` (a HAL entra com elas, como previsto no comentário do arquivo).
- **`src/main.cpp`** reescrito como superloop:
  - `hal::board_early_init()` é a primeira chamada: GPIO18/23/32 HIGH e WiFi/BT
    off, tudo **antes** de `SPI.begin()`.
  - Bring-up: baro, IMU, GPS, SX1276 (`begin()` só configura, não transmite) e
    microSD. Cada `begin()`/`mount()` que falha marca a flag do módulo como falsa e
    o boot segue — nenhum módulo ausente trava o `setup()`.
  - Superloop no `loop()` do Arduino (a loopTask), temporizado a **50 Hz** com
    `vTaskDelayUntil` (Δt = 20 ms). Corpo: alimenta o watchdog, drena a UART do
    GPS, lê os sensores e imprime o diagnóstico a ~4 Hz com a folga do ciclo
    medida no Serial. **Sem lógica de voo** (altitude, pacote, gravação por ciclo
    são das issues 03/06).
  - **TWDT**: a loopTask se inscreve (`esp_task_wdt_add`) no fim do `setup()` e
    alimenta (`esp_task_wdt_reset`) a cada volta — uma subscrição só (superloop),
    contra as duas do ELE3km.

### Decisão de escopo (grill deste ciclo)

- **microSD: só montagem.** `SdLog` ganhou um método `mount()` mínimo (montagem do
  cartão para detectar presença, sem criar arquivo, pré-alocar nem gravar
  cabeçalho). O arquivo pré-alocado, o cabeçalho com o datum (101325 Pa) e a
  escrita por ciclo continuam **inteiramente na issue 06**, que é dona do log
  durável, e o datum na issue 03. Foi a única adição à HAL copiada — não uma
  reescrita de driver.
- **`BootCounter::next()` não é chamado ainda.** O contador é copiado, mas quem o
  usa é a issue 06 (arquivo nomeado pelo contador de boot). Chamá-lo agora só
  incrementaria a NVS a cada boot de bancada sem uso.

### Definition of Done

1. ✅ `pio test -e native` — 5/5 (regressão; a issue não acrescenta núcleo).
2. ✅ `pio run -e heltec_wifi_lora_32_V2` compila (RAM 15,3 %, Flash 27,7 %).
3. ✅ As cinco greps de `DISCIPLINE.md` saem vazias.
4. ✅ Esta seção.
5. ✅ `NEXT.md` atualizado (02 → ✅, ▶ move para a 03).

### Aberto para a bancada (precisa da placa)

- A placa sobe, imprime o diagnóstico de sensores no Serial e não reseta sozinha.
- Folga do ciclo de 50 Hz confirmada no Serial em hardware real.
- Um módulo fisicamente ausente (baro/IMU/GPS/SD/rádio) não trava o boot.
