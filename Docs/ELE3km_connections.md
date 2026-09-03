# ELE3km — Component & Pin Connection List

**Ground truth for firmware.** Regenerated on **2026-07-25** from the actual netlist
(`kicad-cli sch export netlist` on `ELE3km.kicad_sch`, KiCad 9.0.9) and cross-checked
against the net assignments in `ELE3km.kicad_pcb`. Where this file disagrees with
`E22_integration.md`, **this file wins** — that one is the older design note.

Board DRC status: `ELE3km-drc.rpt` → **0 errors, 0 unconnected pads** (the 51 entries in
`DRC.rpt` are silkscreen-overlap warnings only, cosmetic).

---

## ⚠️ Read this before you power the board

### 1. IMU ground is floating — hardware fix required

**U2 (GY-521 / MPU6050) `GND` pad is not connected to board ground.** It is a single-pin net
on both the schematic and the PCB copper, so DRC did not flag it (a one-pin net has nothing
to route against). **The IMU cannot work as built.**

**Fix on the assembled board:** solder an external jumper from the U2 `GND` pad to any board
GND point (U3 pin 2, U1 GND, or the U6 buck `Out-`). Do this before powering up for IMU
testing. *Proper fix for rev. 2: wire `U2.GND` to the GND net in Eeschema and re-route.*

Firmware consequence: the MPU6050 will not ACK on I²C until this jumper exists. Startup
detection must treat a missing IMU as a degraded-but-flyable state (see the PRD), not a fault.

### 2. The onboard SX1276 shares MISO and boots *unselected-by-nothing*

On the Heltec V2 the SX1276 is hard-wired inside the module to **SCK=GPIO5, MOSI=GPIO27,
MISO=GPIO19** — the exact bus the microSD and the E22 sit on — and its **NSS is GPIO18**,
which is broken out to a header pad that this board leaves unconnected (net
`unconnected-(U1-LORA_CS…GPIO18…)`). Nothing pulls GPIO18 high.

**GPIO18 floats at reset → the SX1276 may select itself and drive MISO, corrupting every
SD and E22 transfer.** The very first thing `setup()` does, before `SPI.begin()`:

```cpp
pinMode(18, OUTPUT); digitalWrite(18, HIGH);   // deselect onboard SX1276 — MANDATORY
pinMode(14, OUTPUT); digitalWrite(14, LOW);    // hold SX1276 in reset — belt & braces
```

### 3. LM2596 trimpots

Both bucks ship at ~1.25 V. Set **U6 = 3.30 V** and **U7 = 5.0 V** with no load connected,
*before* plugging in the modules. Do not set U6 above 3.3 V: the sensor/E22 rail is a
separate 3.3 V domain from the ESP32's own 3.3 V (see Power tree), and the I²C pull-ups on
the breakouts would then push above the ESP32's VDD.

### 4. Verify which microSD module you have

If it's the common "Micro SD Card Adapter" with an AMS1117 LDO + 74LVC125/4050 buffer, it
(a) expects **5 V** on VCC — it will brown out on this board's 3.3 V rail — and (b) many
of those buffers **drive MISO even when CS is high**, which contends with the E22 on the
shared bus. You want the bare **3.3 V-only "Micro SD Storage Board"** type here. Check
before soldering.

---

## Component inventory

| Ref          | Part                                        | Function                    | Bus  |
| ------------ | ------------------------------------------- | --------------------------- | ---- |
| U1           | Heltec WiFi LoRa 32 **V2** (ESP32 + SX1276) | MCU (SX1276 stays silent)   | —    |
| U8           | Ebyte **E22-400M30S** (SX1268)              | 1 W 433 MHz radio (only TX) | SPI  |
| U2           | GY-521 (**MPU6050**)                        | IMU                         | I²C  |
| U3           | GY-**BMP280**                               | Barometer                   | I²C  |
| U4           | MicroSD adapter                             | Flight log                  | SPI  |
| U5           | GY-**NEO6MV2**                              | GPS                         | UART |
| U6           | **LM2596** buck #1                          | 3.3 V rail (sensors + E22)  | —    |
| U7           | **LM2596** buck #2                          | 5 V rail (ESP32 only)       | —    |
| J3           | Screw terminal "Bateria"                    | Battery in                  | —    |
| J4           | Screw terminal "chave"                      | Power switch                | —    |
| C1 / C2 / C3 | 1000 µF / 10 µF / 100 nF                    | E22 rail decoupling         | —    |
| R1 / R2      | 10 kΩ                                       | TXEN / RXEN pulldowns       | —    |

`R3` (NRST pull-up) from `E22_integration.md` was **not fitted** — the E22 `NRST` line has no
passive pull-up. Firmware must drive GPIO33 explicitly (see Boot-state contract).

---

## Power tree

```
Battery+ (J3.2) → switch (J4) → J4.2 ─┬─ U6.In+ → U6.Out+ = 3V3_RAIL
                                      └─ U7.In+ → U7.Out+ = 5V_RAIL → U1.5V (J2_2)
Battery- (J3.1) ──────────────────────┴─ U6.In- , U7.In-   (both buck Out- = GND)

3V3_RAIL  →  U2.VCC, U3.VCC, U3.CSB, U4.VCC, U5.VCC, U8.VCC (pads 9+10), C1/C2/C3
5V_RAIL   →  U1.5V only  (ESP32 module regulates its own 3.3 V internally)
```

**The board has two supply voltages, one per buck.** Both LM2596s hang off the same
switched battery node (`J4.2`); they are set to *different* output voltages:

| Buck   | Set to    | Feeds                                              | Silkscreen |
| ------ | --------- | -------------------------------------------------- | ---------- |
| **U6** | **3.3 V** | U2, U3, U4, U5, U8 (pads 9+10), C1/C2/C3           | `3.3v`     |
| **U7** | **5.0 V** | `U1.J2_2` (Heltec 5V pin) — **and nothing else**   | `5v`       |

Nothing on this board runs at 5 V except the Heltec's own input; every peripheral is 3.3 V.
There is no level shifting anywhere, and none is needed.

**Separately — there are also two distinct 3.3 V *sources*, joined only by GND:**

1. `3V3_RAIL` = the U6 buck output → all sensors, SD, E22. KiCad net: `Net-(U3-CSB)`.
2. The Heltec's **internal** 3.3 V regulator, fed from the 5 V rail, which powers the ESP32
   itself. Its output appears on pins `J3_2`/`J3_3` (net `3V3`), which are **tied to each
   other and to nothing else** — it neither feeds the sensors nor is fed by U6.

So the ESP32's VDD and the sensor rail are two regulators that merely happen to sit at the
same voltage. Electrically fine (common ground, both ≈3.3 V), but it means: **if U6 is
misadjusted, the ESP32 keeps running perfectly while every sensor misbehaves.** When
debugging, measure the sensor rail at U8 pad 9, not at the ESP32.

`VEXT` (U1 `J2_3`+`J2_4`) is tied together and otherwise unused.

---

## Shared buses (as actually wired)

| Bus                   | ESP32 pin                        | Devices on it                            |
| --------------------- | -------------------------------- | ---------------------------------------- |
| **VSPI SCK**          | GPIO5                            | U4.SCK, U8.SCK, **+ internal SX1276**    |
| **VSPI MOSI**         | GPIO27                           | U4.MOSI, U8.MOSI, **+ internal SX1276**  |
| **VSPI MISO**         | GPIO19                           | U4.MISO, U8.MISO, **+ internal SX1276**  |
| CS — microSD          | GPIO23                           | U4.CS                                    |
| CS — E22              | GPIO32                           | U8.NSS                                   |
| CS — onboard SX1276   | GPIO18                           | internal to U1; header pad unconnected   |
| **I²C SCL**           | GPIO22                           | U2.SCL, U3.SCL                           |
| **I²C SDA**           | GPIO21                           | U2.SDA, U3.SDA                           |
| **GPS UART**          | GPIO13 = ESP32 TX → GPS RX       | U5.2                                     |
|                       | GPIO17 = ESP32 RX ← GPS TX       | U5.3                                     |

Three chip-selects live on one bus (18 / 23 / 32). Firmware must guarantee exactly one is
low at any instant, and that GPIO18 is *never* low.

---

## Firmware pin contract

Single source of truth — mirror this into exactly one header and never hard-code a pin
anywhere else.

```cpp
// include/pins.h — ELE3km rev.1, from the KiCad netlist (2026-07-25)
#pragma once

// ── Shared VSPI ─────────────────────────────────────────────
constexpr int PIN_SPI_SCK   = 5;
constexpr int PIN_SPI_MISO  = 19;
constexpr int PIN_SPI_MOSI  = 27;

// ── Chip selects (exactly one low at a time) ────────────────
constexpr int PIN_SD_CS     = 23;
constexpr int PIN_E22_NSS   = 32;
constexpr int PIN_LORA_CS   = 18;   // onboard SX1276 — DRIVE HIGH FOREVER
constexpr int PIN_LORA_RST  = 14;   // onboard SX1276 — hold LOW to keep it dead

// ── E22-400M30S (SX1268) ────────────────────────────────────
constexpr int PIN_E22_NRST  = 33;   // output, no external pull-up fitted
constexpr int PIN_E22_BUSY  = 36;   // INPUT ONLY (no pull-up/down available)
constexpr int PIN_E22_DIO1  = 39;   // INPUT ONLY  — IRQ
constexpr int PIN_E22_TXEN  = 25;   // 10k pulldown R1; also the Heltec white LED
constexpr int PIN_E22_RXEN  = 12;   // 10k pulldown R2 — STRAPPING PIN, see below

// ── I²C (MPU6050 + BMP280) ──────────────────────────────────
constexpr int PIN_I2C_SDA   = 21;   // also Heltec Vext control — see caveats
constexpr int PIN_I2C_SCL   = 22;

// ── GPS UART (NEO-6M, 9600 8N1 NMEA by default) ─────────────
constexpr int PIN_GPS_TX    = 13;   // ESP32 transmits → GPS RX pad
constexpr int PIN_GPS_RX    = 17;   // ESP32 receives  ← GPS TX pad

// ── Bus addresses / rates ───────────────────────────────────
constexpr uint8_t ADDR_MPU6050 = 0x68;  // AD0 floating; probe 0x69 too
constexpr uint8_t ADDR_BMP280  = 0x76;  // SDO floating; probe 0x77 too
constexpr uint32_t I2C_HZ      = 100000;
constexpr uint32_t GPS_BAUD    = 9600;
```

RadioLib instantiation (note: `E22_integration.md` shows `setRfSwitchPins(2, 25)` — that is
**stale**, RXEN is GPIO12):

```cpp
SX1268 radio = new Module(PIN_E22_NSS, PIN_E22_DIO1, PIN_E22_NRST, PIN_E22_BUSY);
SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_E22_NSS);
radio.setRfSwitchPins(PIN_E22_RXEN, PIN_E22_TXEN);   // 12, 25
```

### Boot-state contract

Order matters. Everything below happens before any driver is initialised.

| Pin        | Required state at boot / first ms | Why                                                  |
| ---------- | --------------------------------- | ---------------------------------------------------- |
| GPIO12     | **LOW** during reset              | Strapping (MTDI = flash voltage select). R2 does this in hardware; **never drive it high before boot completes**, or the ESP32 comes up with the wrong flash voltage and won't boot. |
| GPIO25     | LOW                               | R1 keeps the E22 PA unkeyed until the driver owns it. |
| GPIO18     | drive **HIGH** immediately        | Deselect onboard SX1276 — floats otherwise.           |
| GPIO14     | drive **LOW** (optional)          | Hold onboard SX1276 in reset so it can never talk.    |
| GPIO23, 32 | drive **HIGH** before `SPI.begin` | No pull-ups on either CS; both float at reset.        |
| GPIO33     | drive HIGH after the reset pulse  | E22 NRST, no pull-up fitted (R3 absent).              |
| GPIO36, 39 | inputs only                       | ESP32 input-only pins — `pinMode(OUTPUT)` is illegal, and internal pull-up/down is unavailable on these. |

---

## Heltec V2 pins that are already spoken for

The Heltec module wires several ESP32 pins internally. Only GPIO5/19/27/25 of these are
deliberately reused by this board; the rest must be left alone.

| ESP32 pin | Heltec internal use   | Status on this board                                    |
| --------- | --------------------- | ------------------------------------------------------- |
| 5, 19, 27 | SX1276 SCK/MISO/MOSI  | **deliberately shared** with SD + E22                   |
| 18        | SX1276 NSS            | must be driven HIGH by firmware (see warning §2)        |
| 14        | SX1276 RESET          | drive LOW to keep the radio dead                        |
| 26, 35, 34| SX1276 DIO0/DIO1/DIO2 | unused, leave as inputs                                 |
| 25        | white user LED        | **reused as E22 TXEN** → the LED lights on every TX burst (free transmit indicator) |
| 21        | **Vext control**      | **reused as I²C SDA** — see caveat below                |
| 4, 15, 16 | OLED SDA / SCL / RST  | OLED is unusable on this board                          |
| 0         | PRG button            | leave floating                                          |
| 1, 3      | USB-serial TX/RX      | keep for the console                                    |

**GPIO21 / Vext caveat.** On the V2, GPIO21 gates the module's Vext output (which powers the
onboard OLED). This board repurposes it as the sensor SDA line, so Vext switches on every
I²C bit. Nothing on this board draws from Vext, so it's harmless — but the Vext gate network
is an extra load on SDA. **If I²C is flaky, that's suspect #1: stay at 100 kHz, don't try
400 kHz.** It's also the real reason the OLED can't be used, so don't spend time on it.

### Genuinely free pins

Only **GPIO2** (strapping — must stay low/floating at boot) and the input-only
**GPIO37 / GPIO38**. Everything else on the headers is either in use or claimed internally.

---

## Per-component pin connections

### U1 — ESP32 (connected pins only)

| Pin              | Net                | Goes to     |
| ---------------- | ------------------ | ----------- |
| GPIO5            | SCK                | U4.5, U8.18 |
| GPIO27           | MOSI               | U4.4, U8.17 |
| GPIO19           | MISO               | U4.3, U8.16 |
| GPIO23           | SD_CS              | U4.6        |
| GPIO32           | E22_NSS            | U8.19       |
| GPIO33           | E22_NRST           | U8.15       |
| GPIO36           | E22_BUSY           | U8.14       |
| GPIO39           | E22_DIO1           | U8.13       |
| GPIO25           | E22_TXEN           | U8.7 + R1.2 |
| GPIO12           | E22_RXEN           | U8.6 + R2.2 |
| GPIO22           | I²C SCL            | U2.SCL, U3.3 |
| GPIO21           | I²C SDA            | U2.SDA, U3.4 |
| GPIO13           | GPS_TX (ESP32 out) | U5.2 (GPS RX) |
| GPIO17           | GPS_RX (ESP32 in)  | U5.3 (GPS TX) |
| 5V (J2_2)        | 5V rail            | U7.Out+     |
| 3V3 (J3_2, J3_3) | tied together only | —           |
| GND (J2_1, J3_1) | GND                | —           |
| VEXT (J2_3, J2_4)| tied together only | —           |

*Broken out and unconnected: GPIO0, 1, 2, 3, 4, 14, 15, 16, 18, 26, 34, 35, 37, 38, RST.*

### U8 — E22-400M30S (pad numbers per Ebyte manual v1.20, confirmed against the symbol)

| Pad                 | Signal | To                        |
| ------------------- | ------ | ------------------------- |
| 1–5, 11, 12, 20, 22 | GND    | GND                       |
| 6                   | RXEN   | GPIO12 (+ R2 pulldown)    |
| 7                   | TXEN   | GPIO25 (+ R1 pulldown)    |
| 8                   | DIO2   | *unconnected (correct)*   |
| 9, 10               | VCC    | 3V3_RAIL (+ C1/C2/C3)     |
| 13                  | DIO1   | GPIO39                    |
| 14                  | BUSY   | GPIO36                    |
| 15                  | NRST   | GPIO33                    |
| 16                  | MISO   | GPIO19                    |
| 17                  | MOSI   | GPIO27                    |
| 18                  | SCK    | GPIO5                     |
| 19                  | NSS    | GPIO32                    |
| 21                  | ANT    | *no PCB trace* → use the module's IPEX/U.FL to a 433 MHz λ/4 whip (~16.5 cm) |

The E22's 1 W burst pulls ≈0.6–1 A from `3V3_RAIL` — the same rail as the SD card and
sensors. This is why the firmware avoids SD writes during a TX burst (PRD §Bus/power).

### U2 — GY-521 / MPU6050 (IMU) — I²C 0x68

| Pin                | To                                                                    |
| ------------------ | --------------------------------------------------------------------- |
| VCC                | 3V3_RAIL                                                              |
| SCL                | GPIO22                                                                |
| SDA                | GPIO21                                                                |
| **GND**            | **⚠️ NOT CONNECTED — external jumper wire to board GND required**     |
| XDA, XCL, AD0, INT | unconnected (AD0 floating → breakout pulldown gives 0x68; probe 0x69) |

INT is not wired, so **the IMU cannot be read interrupt-driven — poll it.**

### U3 — GY-BMP280 (barometer) — I²C 0x76

| Pin         | To                                                    |
| ----------- | ----------------------------------------------------- |
| VCC (1)     | 3V3_RAIL                                              |
| GND (2)     | GND                                                   |
| SCL (3)     | GPIO22                                                |
| SDA (4)     | GPIO21                                                |
| CSB (5)     | tied to VCC → selects I²C mode                        |
| SDD/SDO (6) | unconnected → address 0x76 on most breakouts; probe 0x77 |

Sanity check at startup: BMP280 `CHIP_ID` (reg 0xD0) = **0x58**. A 0x60 there means you
actually have a BME280 (different register map / humidity channel).

### U4 — MicroSD (SPI)

| Pin      | To       |
| -------- | -------- |
| GND (1)  | GND      |
| VCC (2)  | 3V3_RAIL |
| MISO (3) | GPIO19   |
| MOSI (4) | GPIO27   |
| SCK (5)  | GPIO5    |
| CS (6)   | GPIO23   |

No card-detect line is wired — presence can only be inferred from a mount attempt.

### U5 — NEO-6M GPS (UART, 9600 8N1, NMEA)

| Pin     | To                          |
| ------- | --------------------------- |
| VCC (1) | 3V3_RAIL                    |
| RX (2)  | GPIO13 (ESP32 transmits)    |
| TX (3)  | GPIO17 (ESP32 receives)     |
| GND (4) | GND                         |

No PPS line is wired, so there is no hardware time reference — timestamps come from the
ESP32 clock plus GPS time-of-fix from the NMEA sentences.

### U6 — LM2596 buck → 3.3 V rail

`In+` ← switched batt+ (J4.2) · `In-` ← batt− (J3.1) · `Out+` → 3V3_RAIL · `Out-` → GND

### U7 — LM2596 buck → 5 V rail

`In+` ← switched batt+ (J4.2) · `In-` ← batt− (J3.1) · `Out+` → U1.5V · `Out-` → GND

### Connectors & passives

- **J3 (Bateria):** 1 → batt− (to U6.In−, U7.In−) · 2 → batt+ (to J4.1)
- **J4 (chave):** 1 ← J3.2 · 2 → switched batt+ (to U6.In+, U7.In+)
- **C1 (1000 µF) / C2 (10 µF) / C3 (100 nF):** pin 1 → 3V3_RAIL · pin 2 → GND
- **R1 (10 kΩ):** pin 1 → GND · pin 2 → GPIO25 (TXEN)
- **R2 (10 kΩ):** pin 1 → GND · pin 2 → GPIO12 (RXEN) — **required for boot**

---

## Startup self-test checklist (what firmware should report)

| Subsystem | Check                                   | If it fails                                    |
| --------- | --------------------------------------- | ---------------------------------------------- |
| SX1276    | GPIO18 high, GPIO14 low                 | n/a — unconditional, do it first               |
| E22       | NRST pulse, BUSY goes low, RadioLib `begin()` == 0 | Fatal for telemetry; keep logging to SD |
| BMP280    | 0x76/0x77 ACK, `CHIP_ID` == 0x58        | Fall back to GPS altitude                      |
| MPU6050   | 0x68/0x69 ACK, `WHO_AM_I` == 0x68       | **Likely the GND jumper.** Disable INS fallback |
| microSD   | mount + write a probe file              | Telemetry continues without logging            |
| GPS       | any NMEA sentence within ~2 s           | Keep flying on baro altitude until fix         |

---

## Cross-references

- `ELE3km_firmware_PRD.md` — what the firmware must do (this file says where the wires go).
- `E22_integration.md` — original radio design note. **Two stale items:** its
  `setRfSwitchPins(2, 25)` snippet (RXEN is GPIO12) and the optional `R3` NRST pull-up
  (never fitted).
- `ELE3km_BOM_shopping_list.md` — parts and sourcing.
