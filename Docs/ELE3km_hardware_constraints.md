# ELE3km — Hardware Contract & Firmware Mitigations

**What this is:** the single briefing a firmware developer needs before writing code — *where
the wires go* (Part A) and *what the hardware will do to you that the software has to defend
against* (Part B), plus a concrete arbitration design (Part C).

**Authority:** pin data here is condensed from `ELE3km_connections.md`, which is generated
from the KiCad netlist and remains the authority if the two ever disagree. Behaviour
requirements live in `ELE3km_firmware_PRD.md`. This document is the bridge between them.

**Verified:** 2026-07-25 against `ELE3km.kicad_sch` + `ELE3km.kicad_pcb` (KiCad 9.0.9).
Board DRC: 0 errors, 0 unconnected pads.

---

# Part A — Connection reference

## Power tree

```
Battery+ (J3.2) → switch (J4) → J4.2 ─┬─ U6.In+ → U6.Out+ = 3V3_RAIL
                                      └─ U7.In+ → U7.Out+ = 5V_RAIL → U1.5V (J2_2)
Battery- (J3.1) ──────────────────────┴─ U6.In-, U7.In-   (both buck Out- = GND)
```

| Buck   | Set to    | Feeds                                                        | Silkscreen |
| ------ | --------- | ------------------------------------------------------------ | ---------- |
| **U6** | **3.3 V** | U2 IMU, U3 baro, U4 SD, U5 GPS, U8 E22 (pads 9+10), C1/C2/C3 | `3.3v`     |
| **U7** | **5.0 V** | `U1.J2_2` only — the Heltec's 5 V input                      | `5v`       |

Two supply voltages, one per buck. Nothing on the board runs at 5 V except the Heltec's own
input; every peripheral is 3.3 V, so no level shifting exists or is needed.

There are also two separate 3.3 V **sources**, joined only by GND: the U6 buck (peripherals)
and the Heltec's internal regulator fed from the 5 V rail (the ESP32 itself). They are
independent regulators that happen to sit at the same voltage. **This split matters — see
[H1](#h1) and [H3](#h3).**

## Buses

| Bus                 | ESP32 pin                     | Devices                                |
| ------------------- | ----------------------------- | -------------------------------------- |
| VSPI SCK/MOSI/MISO  | 5 / 27 / 19                   | U4 SD, U8 E22, **+ onboard SX1276**    |
| CS — microSD        | 23                            | U4.6                                   |
| CS — E22            | 32                            | U8.19                                  |
| CS — onboard SX1276 | 18                            | internal to U1; header pad unconnected |
| I²C SDA / SCL       | 21 / 22                       | U2 (0x68), U3 (0x76)                   |
| GPS UART            | 13 (ESP32 TX) / 17 (ESP32 RX) | U5.2 / U5.3, 9600 8N1 NMEA             |

## Pin contract

```cpp
// include/pins.h — ELE3km rev.1, from the KiCad netlist (2026-07-25)
#pragma once

constexpr int PIN_SPI_SCK   = 5;
constexpr int PIN_SPI_MISO  = 19;
constexpr int PIN_SPI_MOSI  = 27;

constexpr int PIN_SD_CS     = 23;
constexpr int PIN_E22_NSS   = 32;
constexpr int PIN_LORA_CS   = 18;   // onboard SX1276 — DRIVE HIGH FOREVER
constexpr int PIN_LORA_RST  = 14;   // onboard SX1276 — hold LOW to keep it dead

constexpr int PIN_E22_NRST  = 33;   // output; no external pull-up fitted (R3 absent)
constexpr int PIN_E22_BUSY  = 36;   // INPUT ONLY
constexpr int PIN_E22_DIO1  = 39;   // INPUT ONLY — IRQ
constexpr int PIN_E22_TXEN  = 25;   // 10k pulldown R1; also the Heltec white LED
constexpr int PIN_E22_RXEN  = 12;   // 10k pulldown R2; STRAPPING PIN

constexpr int PIN_I2C_SDA   = 21;   // also Heltec Vext control
constexpr int PIN_I2C_SCL   = 22;

constexpr int PIN_GPS_TX    = 13;   // ESP32 transmits → GPS RX pad
constexpr int PIN_GPS_RX    = 17;   // ESP32 receives  ← GPS TX pad

constexpr uint8_t  ADDR_MPU6050 = 0x68;   // probe 0x69 too
constexpr uint8_t  ADDR_BMP280  = 0x76;   // probe 0x77 too
constexpr uint32_t I2C_HZ       = 100000;
constexpr uint32_t GPS_BAUD     = 9600;
```

```cpp
SX1268 radio = new Module(PIN_E22_NSS, PIN_E22_DIO1, PIN_E22_NRST, PIN_E22_BUSY);
SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_E22_NSS);
radio.setRfSwitchPins(PIN_E22_RXEN, PIN_E22_TXEN);   // 12, 25
```

> `E22_integration.md` shows `setRfSwitchPins(2, 25)` — **stale**. RXEN is GPIO12.

## Boot order (nothing else may run before this)

```cpp
pinMode(PIN_LORA_CS, OUTPUT);  digitalWrite(PIN_LORA_CS, HIGH);  // 1. kill onboard SX1276
pinMode(PIN_LORA_RST, OUTPUT); digitalWrite(PIN_LORA_RST, LOW);  // 2. hold it in reset
pinMode(PIN_SD_CS, OUTPUT);    digitalWrite(PIN_SD_CS, HIGH);    // 3. deselect SD
pinMode(PIN_E22_NSS, OUTPUT);  digitalWrite(PIN_E22_NSS, HIGH);  // 4. deselect E22
// only now: SPI.begin(), then drivers
```

---

# Part B — Hazards the firmware must mitigate

Ordered by how badly they hurt the mission.

<a name="h1"></a>

## H1 — The E22 and the microSD share both the SPI bus *and* the 3.3 V rail

**This is the hazard you identified, and it is worse than "a burst".**

The E22 draws ≈1 A from `3V3_RAIL` at 30 dBm. The microSD, the barometer, the IMU and the
GPS sit on that same rail. They also share VSPI with the radio.

**The number that changes everything.** At the originally proposed **SF11 / BW125 / CR4/8**, a
20-byte packet is not a burst — it is *nearly a full second* of continuous 1 A draw:

| SF     | 20 B full packet | 10 B altitude-only | Duty @ 2 s cadence | Duty @ 5 s cadence |
| ------ | ----------------:| ------------------:| ------------------:| ------------------:|
| 7      | 78 ms            | 54 ms              | 3.9%               | 1.6%               |
| 8      | 140 ms           | 91 ms              | 7.0%               | 2.8%               |
| 9      | 247 ms           | 181 ms             | 12.3%              | 4.9%               |
| 10     | 494 ms           | 362 ms             | 24.7%              | 9.9%               |
| **11** | **987 ms**       | 725 ms             | **49.4%**          | 19.7%              |
| 12     | 1712 ms          | 1188 ms            | 85.6%              | 34.2%              |

*(Semtech airtime formula, BW 125 kHz, CR 4/8, CRC on, explicit header, 8-symbol preamble,
LowDataRateOptimize forced on at SF11/SF12. Recompute if you change any parameter.)*

At SF11 with a 2 s cadence the PA is keyed **half of all wall-clock time**. There is no
"between the bursts" to hide an SD write in. Three consequences follow, and they all point
the same way.

### H1a — SD corruption

An SD write interrupted by a rail sag doesn't just lose that record. If the sag lands during
a FAT or directory-entry update, **the whole log file becomes unreadable** — you lose the
flight data you flew to collect.

**Mitigations (all of them, not a choice):**

1. **Serialize by policy, not by luck.** One "bus + rail owner" at a time (see Part C). A TX
   may not start while an SD write is in flight; an SD write may not start while TX is keyed.
2. **Buffer in RAM.** Sensor records go into a ring buffer; the SD writer drains it during
   radio-idle windows. Never write straight through from the sample loop.
3. **Fixed-size binary records, append-only, pre-allocated file.** A truncated tail must
   still leave every earlier record parseable. Never rewrite in place.
4. **Flush on a bounded schedule, in an idle window** — `flush()` is itself a FAT write and
   is the *most* dangerous operation to have coincide with TX.
5. **New file per boot** (incrementing index) so a mid-flight reset never clobbers the
   previous file.

### H1b — Lowering SF is a hardware mitigation, not just a speed choice

**This is what drove the SF decision now recorded in the PRD:** use **SF7–SF9 during flight**
and reserve **SF12 for the post-landing beacon**.

- Link budget makes SF11 unnecessary in flight. Free-space path loss at 8 km / 433 MHz is
  ≈103 dB. With +30 dBm TX and a modest Yagi at the ground station, SF7's ≈−123 dBm
  sensitivity leaves tens of dB of margin even after generous allowance for a tumbling
  rocket's antenna orientation. **Line-of-sight is the easy part of this mission.**
- SF7 at 1 Hz is **3.9% duty** — which opens ~920 ms of clean radio-idle time every second
  for SD writes and sensor reads. The interleaving in Part C becomes trivial instead of
  impossible.
- SF12 is worth its 1.7 s airtime **only after landing**, where the rocket is on the ground
  under foliage and the link is genuinely hard — and where a 30–60 s cadence makes the duty
  cycle irrelevant.

Make SF a per-flight-phase parameter, not a constant. **The settled choice is SF8 in flight
at 1 Hz and SF12 for the landed beacon — see [C1](#c1).**

### H1c — Duty cycle, heat and battery

At 30 dBm the E22-400M30S runs hot; Ebyte's own guidance is to keep the transmit duty cycle
bounded. SF11 at a 2 s cadence (≈50% duty) is a thermal and battery problem independent of
the SD issue: ≈0.157 mAh from a 2S LiPo *per packet*, ≈3.3 J at the rail.

**Mitigation:** enforce a hard duty-cycle ceiling in firmware (a token/credit budget over a
rolling window) that the cadence logic cannot exceed, whatever the flight phase asks for.
Make it a compile-time constant that is checked, not a comment.

<a name="h2"></a>

## H2 — GPIO18 floats and the onboard SX1276 can hijack MISO

The Heltec's SX1276 is wired internally to the *same* SCK/MOSI/MISO (5/27/19). Its NSS is
GPIO18, broken out to a header pad this board leaves unconnected — **nothing pulls it high**.

If GPIO18 floats low, the SX1276 selects itself and drives MISO against the SD and the E22.
Symptom: random SPI corruption that looks like a flaky card or a bad radio.

**Mitigation:** drive GPIO18 HIGH and GPIO14 LOW as the very first statements in `setup()`,
before `SPI.begin()`. Never reconfigure them. Neither SD_CS (23) nor E22_NSS (32) has a
pull-up either — drive both HIGH in the same block.

<a name="h3"></a>

## H3 — A TX sag resets the *peripherals*, silently

The ESP32 is powered from the 5 V buck through its own regulator, so an E22 sag on
`3V3_RAIL` does **not** brown out the MCU. That is good for control — and bad for trust:
**the firmware keeps running perfectly while its sensors reboot underneath it.**

- **BMP280:** a reset returns it to sleep mode with default oversampling. It will keep
  ACKing on I²C and returning *plausible-looking but wrong* data. This is the silent killer.
- **NEO-6M:** a reset loses the fix. Cold start is ~30 s — potentially the entire descent.
- **MPU6050:** a reset returns it to sleep; readings freeze rather than error.

**Mitigations:**

1. **Periodically re-read configuration registers**, not just data. If the BMP280's `CTRL_MEAS`
   no longer matches what you wrote, re-initialise and mark the samples in between suspect.
2. **Watch for the GPS NMEA stream restarting** (or the fix dropping to zero satellites right
   after a TX) and treat it as a device reset — re-apply any UBX configuration.
3. **Log a per-sensor reinit counter.** If it climbs during flight, the rail is sagging and
   the next board revision needs a separate buck for the radio.
4. Confirm the GPS breakout's backup cell works, so a reset gives a warm start instead of a
   cold one.

Note that the shared *battery* node still couples the two bucks: a 1 A draw on the 3.3 V
buck pulls ~0.5 A from the pack, and a small LiPo with high internal resistance can sag the
5 V rail through the screw terminals. So an ESP32 brownout is unlikely but not impossible —
see H4.

<a name="h4"></a>

## H4 — A mid-flight reset destroys the altitude reference

Barometric altitude is relative to a ground-pressure reference captured at startup. **If the
board resets at 3 km and re-zeros, it will report an altitude of 0 while at 3 km** — and
altitude is the priority payload of the entire mission.

**Mitigations:**

1. Persist the ground reference pressure in NVS at arm/boot, together with a boot counter.
2. On boot, if a stored reference exists and a flight is marked in progress, **reuse it —
   do not re-zero.**
3. Log the boot counter in every record so the analyst can see a reset happened.
4. Sanity-check on reuse: if the stored reference implies an absurd altitude, fall back to
   GPS altitude and flag it in the packet.

<a name="h5"></a>

## H5 — The ungrounded IMU can take the I²C bus down with it

`U2.GND` is floating (see `ELE3km_connections.md`; needs a jumper wire at assembly). Until
that wire exists, the MPU6050 is parasitically powered through its ESD diodes from the SDA/SCL
pull-ups. A part in that state doesn't cleanly "not respond" — **it can hold SDA low and hang
the bus, taking the barometer with it.**

**Mitigations:**

1. **I²C bus recovery routine:** on a transaction timeout, release the peripheral, manually
   toggle SCL ~9 times to clock out a stuck slave, issue a STOP, re-init the driver.
2. **Hard timeout on every I²C transaction** — never an unbounded blocking read.
3. **Probe order matters:** detect the barometer *first*. If the IMU probe hangs the bus,
   you at least know the baro was healthy.
4. Treat "IMU absent" as a normal degraded mode: disable the INS fallback, keep flying on
   GPS + baro (PRD §Graceful degradation).

<a name="h6"></a>

## H6 — E22 BUSY and DIO1 sit on ESP32 input-only pins with a known glitch erratum

`BUSY = GPIO36` and `DIO1 = GPIO39` are input-only (no `pinMode(OUTPUT)`, and **no internal
pull-up or pull-down is available on these pins**). There is also a documented ESP32 erratum
in which GPIO36/GPIO39 can register a brief spurious low pulse when the SAR/ADC power domain
is switched. On this board that would mean a phantom radio interrupt or a false "radio ready".

**Mitigations:**

1. **Do not use ADC1, the Hall sensor, the touch peripheral, or the ULP.** This board has no
   analog inputs, so there is no reason to call `analogRead()` / `hallRead()` / `touchRead()`
   anywhere. Make that a review rule.
2. **Never trust a DIO1 edge on its own** — on interrupt, read the radio's IRQ status
   register and ignore the event if no flag is set. RadioLib's `getIrqStatus()` is the check.
3. **Never wait on BUSY forever.** Bounded timeout + a recovery path (NRST pulse, re-init).
4. Since GPIO33 (NRST) has no pull-up fitted, drive it explicitly — do not rely on the line
   idling high.

<a name="h7"></a>

## H7 — RXEN is on GPIO12, a strapping pin

GPIO12 (MTDI) selects the flash voltage at reset and **must be low when reset is released**,
or the ESP32 comes up with the wrong flash voltage and does not boot. R2 (10 kΩ to GND)
guarantees this in hardware.

**Mitigations:** never drive GPIO12 high before boot completes; hand it to RadioLib via
`setRfSwitchPins()` and never touch it directly afterwards. If you ever add a bootloader
hold or an early GPIO init, GPIO12 is excluded from it. Do not fit an external pull-up to
GPIO12 for any reason.

<a name="h8"></a>

## H8 — GPIO25 is both TXEN and the Heltec's white LED

Every Heltec example on the internet blinks GPIO25. On this board **blinking the LED keys the
1 W power amplifier.**

**Mitigation:** no LED code anywhere. GPIO25 belongs to RadioLib. (The upside: the white LED
becomes a free transmit indicator — if it's solid on when you expect idle, TXEN is stuck and
the PA is cooking.)

<a name="h9"></a>

## H9 — GPIO21 is both SDA and the Heltec's Vext control

On the V2, GPIO21 gates the module's Vext output. This board reuses it as sensor SDA, so
Vext switches on every I²C bit. Nothing draws from Vext here, so it is harmless — but the
Vext gate network is extra load on SDA.

**Mitigations:** run I²C at **100 kHz** and do not try 400 kHz. If I²C is flaky, this is
suspect #1 after H5. The onboard OLED is unusable for this reason; don't spend time on it.

<a name="h10"></a>

## H10 — TXEN and RXEN high simultaneously can destroy the module

Both high puts the E22's RF switch in an undefined state and can route PA output into the
LNA. A single firmware bug here is a dead 1 W module.

**Mitigations:** only RadioLib drives pins 12 and 25, via `setRfSwitchPins()`. No direct
`digitalWrite()` on either, ever. If you add any manual RF-switch handling, assert mutual
exclusion in code.

**Related operational rule (firmware cannot detect this):** never transmit without the
antenna connected — 1 W into an open circuit can destroy the PA. Pad 21 (ANT) has no PCB
trace; the RF path is the module's IPEX connector to a 433 MHz λ/4 whip (~16.5 cm).

<a name="h11"></a>

## H11 — The IMU saturates during boost

The MPU6050 maxes out at ±16 g and ±2000 °/s. A rocket accelerating to 800–1000 km/h will
**exceed ±16 g during the burn**, and a spin-stabilised airframe can exceed the gyro range.

A saturated reading is not noise — it is a hard clip. Integrating a clipped 16 g when the
true value is 30 g produces a large, systematic velocity *under*-estimate, and the Kalman
filter has no way to know.

**Mitigations:**

1. **Detect saturation** (reading at or adjacent to full scale) and flag the sample.
2. **Do not feed clipped samples to the predict step as if valid** — either skip the update
   or inflate the process covariance so the filter distrusts them.
3. Configure the full-scale range to ±16 g / ±2000 °/s from the start.
4. Set the DLPF sensibly: rocket-motor vibration aliases into the accelerometer if the
   filter is wide open.
5. Log the raw values *and* the saturation flag, so post-flight analysis can see it.

<a name="h12"></a>

## H12 — Barometric altitude is unreliable transonic

At 800–1000 km/h the airframe is transonic. A bare BMP280 on a breakout, without a properly
designed static port, will read badly wrong pressure through the shock regime. The PRD makes
the barometer the *primary* altitude source, which is right for coast, descent and landing —
and wrong for boost.

**Mitigations:**

1. Downweight the baro in the estimator when vertical acceleration or velocity is high;
   lean on the GPS and inertial channel through boost.
2. Reject impossible pressure steps (rate-of-change limit) rather than feeding them in.
3. Enable the BMP280's internal IIR filter, but remember it adds lag during fast climbs.
4. Log raw pressure alongside derived altitude so the flight can be re-derived afterwards.

<a name="h13"></a>

## H13 — SD cards stall unpredictably

An SD card can block for 100 ms or more during internal garbage collection or wear
levelling. If the main loop writes synchronously, that stall drops IMU samples and puts a
hole in the Kalman predict step — exactly during the most dynamic part of the flight.

**Mitigations:**

1. **Timestamp samples at acquisition, never at write time.**
2. Ring-buffer records; drain to SD in bounded chunks, ideally from a separate task pinned
   away from the sample loop.
3. Size the buffer to survive a worst-case stall plus a full SF12 transmission.
4. On buffer overflow, **drop the oldest log records — never delay a sensor read or a
   telemetry packet.** Losing log resolution is recoverable; losing the recovery link is not.
5. Write in 512-byte aligned blocks to minimise the number of card transactions.

<a name="h14"></a>

## H14 — Boot-time float on the chip selects

Neither SD_CS (23) nor E22_NSS (32) has a pull-up, and the ESP32 leaves them high-Z for the
first few hundred milliseconds of the bootloader. Spurious selection during that window can
put an SD card into an odd state.

**Mitigation:** the Part A boot block — drive all four CS/RST lines to their safe state as
the first statements in `setup()`. If a card refuses to mount on power-up but mounts after a
soft reset, this is the cause.

---

# Part C — The chosen design

Requirements this serves: **live telemetry throughout the whole flight** (not only after
landing), **flight-log integrity**, and **robustness** — this is a university competition
rocket, so a lost log or a lost vehicle is a lost season.

Robustness here comes from **three independent layers**. None depends on the others working,
which is what makes the set robust rather than any one layer being excellent:

| Layer                             | Protects against                        | Section   |
| --------------------------------- | --------------------------------------- | --------- |
| Time-division of radio vs SD      | the 1 A TX draw corrupting an SD write  | [C3](#c3) |
| Pre-allocated contiguous log file | FAT corruption destroying the whole log | [C4](#c4) |
| Ground-station raw packet log     | total loss of the vehicle               | [C5](#c5) |

<a name="c1"></a>

## C1 — Radio: two configurations, switched by flight phase

**Two configs, one transition.** Deliberately not five phases: each extra state is another
chance to false-trigger on top of a rocket.

| Phase                      | SF   | Cadence  | Payload     | Airtime | Duty  |
| -------------------------- | ---- | -------- | ----------- | -------:| -----:|
| **FLIGHT** (pad → landing) | SF8  | 1 Hz     | full (20 B) | 140 ms  | 14 %  |
| **LANDED**                 | SF12 | 1 / 20 s | full (20 B) | 1712 ms | 8.6 % |

Common to both: 433 MHz, BW 125 kHz, CR 4/8, CRC on, explicit header, +30 dBm.

**Why SF8 in flight and not SF11.** SF11 is what *prevents* continuous in-flight
telemetry: at 987 ms airtime it cannot be sent faster than roughly once per 8–10 s without
an unacceptable duty cycle. SF8 costs 140 ms, so **1 Hz for the entire flight** fits in a
14 % duty cycle. The in-flight link is line-of-sight — ≈103 dB free-space path loss at
8 km / 433 MHz against +30 dBm and a ground Yagi — so SF8 keeps tens of dB of margin even
allowing for a tumbling airframe's antenna orientation. Spending SF11's airtime on the easy
part of the link buys nothing and costs the telemetry rate.

**Why SF12 only after landing.** On the ground, under foliage, antenna possibly horizontal
against wet earth, the link is genuinely hard and SF12's extra ≈10 dB earns its 1.7 s. At a
20 s cadence the duty cycle is irrelevant. This is the *only* place the high SF pays.

**Disable WiFi and Bluetooth explicitly** (`WiFi.mode(WIFI_OFF); btStop();`). Neither is used;
both cost 100–250 mA in bursts and add CPU interrupts.

<a name="c2"></a>

## C2 — Landing detection, and the apogee trap

> **⚠️ A naive "low motion" detector fires at apogee and kills your descent telemetry.**
> 
> In free fall the accelerometer reads ≈**0 g**, steadily. A detector based on *low variance*
> of acceleration therefore sees the same signature at apogee as at rest, switches to SF12
> mid-flight, and you lose live telemetry for the entire descent — exactly the requirement
> this design exists to satisfy.
> 
> At rest the accelerometer reads ≈**1 g**, not 0. That distinction is the whole test.

Require **all four** conditions simultaneously before declaring LANDED:

1. `|a| ≈ 1 g` — magnitude near one gravity, *not merely stable*.
2. Barometric altitude stable within a small band for N consecutive seconds.
3. Altitude near the stored ground reference (H4).
4. A minimum elapsed time since liftoff detection.

**The transition is one-way:** once LANDED, only a reset leaves it. A false negative costs
battery; a false positive costs the flight. Log the phase in every record and put it in the
packet so the ground crew can see a bad transition immediately.

<a name="c3"></a>

## C3 — The three-state cycle

One owner of the 3.3 V rail at a time. Note carefully **which** resource is scarce:

> During the airtime the **SPI bus is already free** — the payload is written to the E22's
> buffer *before* TX is keyed, and the radio then transmits autonomously and raises DIO1 when
> done. No SPI traffic is needed while the PA is on. **The constraint is the power rail, not
> the bus.** (Which is also the rev-2 hardware fix: give the E22 its own buck and the whole
> conflict disappears.)

Use RadioLib's non-blocking `startTransmit()`, not `transmit()`.

```
  ├──── RADIO_TX (PA keyed, rail dirty) ────┤├─ SD_WRITE ─┤├──── IDLE (rail clean) ────┤
              140 ms  (SF8, 20 B)               ~60 ms                ~800 ms

  sensors sample continuously across all three states (I²C + UART only, never SPI)
                    │                              │
          SPI free but rail dirty         radio in standby (~2 mA)
```

```
                 ┌───────────────────────────────────────────┐
   sensors ──────► RAM ring buffer (fixed-size records)       │
   (never blocked)└──────────────┬────────────────────────────┘
                                 │ drained only in SD_WRITE
                                 ▼
   ┌────────────┐  granted   ┌────────┐  granted   ┌──────────┐
   │  SD_WRITE  │◄───────────┤  IDLE  ├───────────►│ RADIO_TX │
   └─────┬──────┘            └────────┘            └────┬─────┘
         │  blocks written                TX done (DIO1 + IRQ status confirmed)
         └───────────────► IDLE ◄───────────────────────┘
```

**Rules:**

1. **No SD write while `RADIO_TX` is active** — it queues, even though the bus is free.
2. **A TX request never preempts an in-flight SD write.** Single-digit-ms delay to a packet
   costs nothing.
3. `RADIO_TX` ends only when the DIO1 interrupt is *confirmed against the radio's IRQ status
   register* (H6), or a timeout fires.
4. **The states are events with variable duration, not fixed time slices.** The ring buffer
   absorbs card jitter; do not build a hard-real-time slot scheduler.
5. The sensor loop is outside this arbitration entirely — I²C and UART only, never SPI, and
   it must never block on the arbiter. **You cannot pause the IMU for an airtime**; the
   Kalman predict step needs continuity.
6. On buffer overflow, **drop the oldest log records** — never delay a sensor read or a
   telemetry packet.
7. Chip selects are asserted only by the current owner, inside SPI transactions. GPIO18 stays
   HIGH permanently and is not part of the arbitration.
8. **Do not cut the E22's VCC** between transmissions. There is no load switch on this board,
   and standby is already ~2 mA. Power-cycling costs a re-init and the NRST has no pull-up.

**Buffer sizing.** A 64-byte record at 100 Hz is 6.4 KB/s. The buffer must cover the longest
radio-busy window plus a worst-case card stall: SF12's 1712 ms + 300 ms ≈ 13 KB. **Allocate
32 KB** (≈5 s of margin) — trivial against the ESP32's ~300 KB of usable SRAM.

<a name="c4"></a>

## C4 — Log integrity: pre-allocated contiguous file

The failure mode that destroys an entire flight is not a lost record — it is an interrupted
FAT or directory-entry write. Remove that failure mode instead of scheduling around it.

**Pre-allocate a large contiguous file at boot (`SdFat::preAllocate()`), then write only raw
512-byte data blocks into it during flight. Do not touch the FAT or the directory entry until
the flight ends.** The metadata is written exactly twice — once at creation, once at close.
A power loss mid-flight can then corrupt nothing but the recorded file *length*; every data
block is already on the card.

Supporting decisions:

- **Use SdFat, not the Arduino `SD` library.** `SD` supports neither pre-allocation nor
  contiguous files, which are the entire mechanism above.
- **Fixed 64-byte binary records**, each with a magic marker, sequence number and CRC, so a
  post-flight tool can scan the raw file and recover every valid record even when the file
  length is wrong.
- **Header block** at offset 0: magic, format version, boot counter, ground reference
  pressure (H4), and the pin/config revision.
- **New file per boot**, incrementing index — a mid-flight reset never clobbers the previous
  file.
- Write in **512-byte aligned blocks** to minimise card transactions.
- Timestamp at **acquisition**, never at write time (H13).

<a name="c5"></a>

## C5 — The ground station is the second copy

**The ground station must append every received packet, raw, with a reception timestamp, to
disk.** This costs one line of code in the receiver and buys a copy of the flight data that
depends on neither the SD card nor physical recovery of the vehicle.

If the rocket burns, lands in water, or is never found, a 1 Hz stream of position, altitude,
vertical velocity and position-source still reconstructs the flight. For a university
competition that is the difference between a lost season and a written-up result. Size the
packet with this in mind: it is not only a tracking aid, it is the backup dataset.

<a name="c6"></a>

## C6 — Budget check

| Quantity                              | FLIGHT (SF8, 1 Hz) | LANDED (SF12, 1/20 s) |
| ------------------------------------- | ------------------:| ---------------------:|
| TX duty cycle                         | 14 %               | 8.6 %                 |
| Mean current, 3.3 V rail (radio only) | ≈140 mA            | ≈86 mA                |
| Mean PA dissipation                   | ≈0.32 W            | ≈0.20 W               |
| Total draw from a 2S LiPo             | ≈200 mA            | ≈140 mA               |

A 1000 mAh 2S pack gives roughly 5 h — which matters for surviving the *search*, not the
flight. Enforce the duty ceiling as a checked constant (H1c), not a comment.

## What this design deliberately does not do

- **No SD writes during airtime**, even though the bus is free. The rail is the constraint.
- **No cutting the E22's power** between packets.
- **No SF11 anywhere.** It is the worst of both worlds: too expensive for flight, weaker than
  SF12 for the beacon.
- **No fixed time-slice scheduler.** Event-driven states plus a ring buffer, so a 300 ms card
  stall degrades resolution instead of breaking the cycle.

---

# Part D — Priority summary

| #   | Hazard                            | If ignored                                     | Cost to fix in firmware     |
| --- | --------------------------------- | ---------------------------------------------- | --------------------------- |
| H1  | TX vs SD on shared bus + rail     | Corrupted flight log; possibly unreadable file | Medium — Part C + SF choice |
| H2  | GPIO18 floating                   | Random SPI corruption, misdiagnosed for days   | **Two lines**               |
| H3  | Peripherals reset silently on sag | Plausible but wrong data; GPS cold start       | Medium                      |
| H4  | Reset loses baro reference        | Reports 0 m at 3 km — mission payload wrong    | Low — NVS + boot counter    |
| H5  | Ungrounded IMU hangs I²C          | Barometer lost too; total sensor blackout      | Low — recovery + timeouts   |
| H6  | GPIO36/39 glitches                | Phantom IRQs, false "radio ready"              | Low — validate + timeouts   |
| H7  | GPIO12 strapping                  | Board does not boot                            | **Discipline only**         |
| H8  | GPIO25 = LED = TXEN               | Accidentally keying a 1 W PA                   | **Discipline only**         |
| H9  | GPIO21 = Vext                     | Flaky I²C                                      | **One constant (100 kHz)**  |
| H10 | TXEN+RXEN both high               | Destroyed radio module                         | **Discipline only**         |
| H11 | IMU saturation in boost           | Silently wrong velocity/position               | Low — flag + covariance     |
| H12 | Transonic baro error              | Wrong altitude exactly during boost            | Medium — estimator gating   |
| H13 | SD stalls                         | Gaps in IMU data at the worst moment           | Medium — buffering          |
| H14 | CS float at boot                  | Card fails to mount on cold power-up           | **Part of the boot block**  |

Five of these cost nothing but discipline (H2, H7, H8, H9, H10) and are the ones most likely
to burn a day of debugging or a module. Fix them on the first commit.

### Addenda — added 2026-07-29

<a name="h15"></a>

#### H15 — E22 TX transient can brown-out the ESP32 through the shared battery node

The two bucks (U6 → 3.3 V, U7 → 5 V) share the same battery node at J4.2. When the E22 keys
its PA, U6 demands ≈0.45 A from the pack (1 A × 3.3 V ÷ 7.4 V). That current flows through
the same screw terminals, wires and switch that feed U7. The voltage drop across the parasitic
resistance of those conductors **pulls the input of U7 down with it**.

The LM2596 regulates with ≈1.5 V dropout — so U7 needs ≥6.5 V input to hold 5.0 V output.
With a fresh 2S pack at 8.4 V the margin is huge. But late in a multi-hour beacon, with the
pack at ≈6.6 V and internal resistance climbing, a 0.3–0.5 V transient sag can push U7's
input below regulation. The Heltec's internal 3.3 V LDO then follows the sag, and the ESP32's
brown-out detector (default threshold ≈2.44 V) fires — **clean reset, mid-beacon**.

**Why it matters:** a reset during the beacon phase is not catastrophic (NVS preserves the
barometric reference and flight phase; the boot sequence creates a new log file), but **each
reset costs 1–30 s of silence** plus a GPS re-acquisition delay if the backup battery is dead.
Repeated resets in a boot-loop degrade the beacon to near-uselessness exactly when the recovery
team depends on it most.

**Hardware mitigations (ordered by impact / difficulty):**

1. **Bulk capacitor on the battery node.** Place a **470–1000 µF / 16 V** electrolytic between
   J4.2 and GND, as close to the screw terminals as physically possible. It serves as an
   energy reservoir for the TX transient — the peak current comes from the cap instead of from
   the battery, giving the bucks time to react. Cost: negligible. Difficulty: one solder joint.

2. **Low-resistance wiring.** Use ≥22 AWG (preferably 20 AWG) wire between the battery and J3,
   and ensure screw terminals are firmly tightened. Every centimetre of thin wire and every
   loose contact adds 10–50 mΩ of parasitic resistance. The switch at J4 should be a robust
   contact — a micro-switch is not adequate for peak currents of this magnitude.

3. **Quality LiPo with low internal resistance.** Use a 2S pack of ≥1000 mAh with a ≥20 C
   discharge rating (brands such as Tattu, CNHL). Typical internal resistance is 20–40 mΩ per
   cell vs. 100–200 mΩ for generic packs. Charge fully and keep at ambient temperature before
   flight.

4. **Local decoupling on the 5 V rail.** Add **100–220 µF / 10 V** near the Heltec's 5 V input
   pin (U1.J2_2). This gives the Heltec's internal regulator a local energy reserve, filtering
   transients that propagate through the battery node from U6's load step.

5. **Separate buck for the E22 (rev. 2 only).** A dedicated buck for the E22 eliminates the
   rail contention entirely — H1 and the full C3 arbitration cease to exist. This is a board
   redesign, not a field fix.

**Firmware mitigation:** configure the ESP32 brown-out detector at the **lowest threshold
(2.43 V)** explicitly in firmware (`CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_3` in sdkconfig). This
maximises the operating margin before a reset fires. The existing NVS persistence (H4), boot
counter, and new-file-per-boot strategy already absorb clean resets — a BOD reset enters the
same recovery path as a watchdog reset. **Do not disable the BOD:** the NVS writes that
guarantee flight continuity depend on the flash operating at valid voltage — silent corruption
of NVS at marginal voltage is worse than a clean reset.

<a name="h16"></a>

#### H16 — EMI from the 1 W PA onto I²C lines and GPS receiver

The E22 transmits +30 dBm at 433 MHz on an antenna centimetres from the I²C bus (SDA/SCL,
100 kHz, unshielded) and the NEO-6M GPS receiver (sensitivity ≈ −130 dBm at 1575 MHz). The
160 dB difference between transmitted power and GPS sensitivity means even small coupling paths
matter. The 3rd harmonic of 433 MHz (1299 MHz) falls near the GPS L1 band.

On the I²C bus, RF energy can induce glitches that the bus interprets as bits — a different
root cause from H5 (stuck slave) but with the same symptom (transaction failure or corrupt
data). The existing defences (timeout, bus recovery, config re-verification) handle the
consequence, but the root cause is distinct and worth tracking separately.

**The firmware should NOT suspend I²C reads during TX.** That would cost ≈14 IMU samples per
second (14% at 100 Hz) and create systematic holes in the Kalman predict step — exactly what
the two-core architecture was designed to prevent. The existing I²C error detection and
recovery path is the correct defence.

**Diagnostic:** correlate I²C error timestamps with TX timestamps in post-flight log analysis.
If errors cluster around TX windows, the rev. 2 board needs shielding or separated routing.
No additional firmware logic is needed — the reinit counter per sensor and the scheduler's TX
timestamps already provide the data.

**Assembly mitigations:**

1. Route the E22 antenna (433 MHz, 16.5 cm whip) as far as physically possible from the GPS
   breakout and the I²C wiring.
2. If layout permits, interpose a ground plane (aluminium foil + Kapton tape) between the E22
   module and the sensor breakouts.
3. Twist the I²C wires (SDA + SCL together) to reduce common-mode RF pickup.
4. Keep the GPS antenna with a clear sky view, not directly adjacent to the E22 antenna.

| #   | Hazard                            | If ignored                                     | Cost to fix in firmware     |
| --- | --------------------------------- | ---------------------------------------------- | --------------------------- |
| H1  | TX vs SD on shared bus + rail     | Corrupted flight log; possibly unreadable file | Medium — Part C + SF choice |
| H2  | GPIO18 floating                   | Random SPI corruption, misdiagnosed for days   | **Two lines**               |
| H3  | Peripherals reset silently on sag | Plausible but wrong data; GPS cold start       | Medium                      |
| H4  | Reset loses baro reference        | Reports 0 m at 3 km — mission payload wrong    | Low — NVS + boot counter    |
| H5  | Ungrounded IMU hangs I²C          | Barometer lost too; total sensor blackout      | Low — recovery + timeouts   |
| H6  | GPIO36/39 glitches                | Phantom IRQs, false "radio ready"              | Low — validate + timeouts   |
| H7  | GPIO12 strapping                  | Board does not boot                            | **Discipline only**         |
| H8  | GPIO25 = LED = TXEN               | Accidentally keying a 1 W PA                   | **Discipline only**         |
| H9  | GPIO21 = Vext                     | Flaky I²C                                      | **One constant (100 kHz)**  |
| H10 | TXEN+RXEN both high               | Destroyed radio module                         | **Discipline only**         |
| H11 | IMU saturation in boost           | Silently wrong velocity/position               | Low — flag + covariance     |
| H12 | Transonic baro error              | Wrong altitude exactly during boost            | Medium — estimator gating   |
| H13 | SD stalls                         | Gaps in IMU data at the worst moment           | Medium — buffering          |
| H14 | CS float at boot                  | Card fails to mount on cold power-up           | **Part of the boot block**  |
| H15 | TX transient browns out ESP32     | Repeated resets degrade beacon to silence      | Low — BOD config + NVS      |
| H16 | EMI from PA onto I²C / GPS        | Spurious I²C errors correlated with TX         | **Assembly + diagnostic**   |

---

## Cross-references

- `ELE3km_connections.md` — netlist-accurate wiring, full per-component pin tables, and the
  IMU GND jumper assembly action.
- `ELE3km_firmware_PRD.md` — required behaviour, module structure, testing strategy. **Note:
  the SF11 baseline was replaced by the phase-switched SF8/SF12 scheme in [C1](#c1).**
- `E22_integration.md` — original radio design note. Two stale items: `setRfSwitchPins(2, 25)`
  (RXEN is GPIO12) and the optional `R3` NRST pull-up (never fitted).
- `ELE3km_BOM_shopping_list.md` — parts and sourcing.
