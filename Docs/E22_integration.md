# E22-400M30S Integration — ELE3km Flight Computer

> **ON HOLD (2026-08-30).** The E22 is being removed; telemetry moves to the
> onboard SX1276 (915 MHz) only. See `ELE3km_drop_e22_single_radio.md` for the
> decision and rationale. This file is kept for the record and for a possible
> future board respin — its wiring/pinout is still accurate for the rev.1 board.

Adds a 1 W / 433 MHz LoRa radio (Ebyte **E22-400M30S**, SX1268) as an external
radio on the existing Heltec ESP32 LoRa V2.0 (U1). Shares the VSPI bus already
used by the onboard SX1276 + microSD, uses the last free GPIOs, and adds the
decoupling needed so the 1 W TX bursts don't sag any rail.

## ⚠️ Two things to get right before wiring

1. **Power the E22 at 3.3 V — NOT 5 V.** On the E22 modules the digital I/O
   high level follows VCC. The ESP32 GPIOs are 3.3 V. Powering the E22 at 5 V
   would put 5 V on SPI/BUSY/DIO lines shared with the 3.3 V ESP32 → damage.
   Feed VCC from a **3.3 V rail**. Confirm your U6 (LM2596) sensor rail is set
   to 3.3 V; if it is, tap it. If it's not 3.3 V, give the E22 its own 3.3 V
   buck (an LDO cannot supply the ~1 A TX burst).

2. **RXEN is on GPIO12, a boot-strapping pin** (flash-voltage select — it MUST
   be low at boot, or the ESP32 won't boot). The 10 kΩ pull-down (R2 below)
   guarantees that. Do not omit it.

## Connection table (with datasheet pad numbers)

Pad numbers confirmed from Ebyte E22-400M30S User Manual v1.20.
**Total: 22 pads + IPEX antenna connector.**

| Pad | E22 signal | Connects to           | ESP32 GPIO | Notes                          |
|----:|------------|-----------------------|-----------:|--------------------------------|
| 1–5 | GND        | GND                   | —          | ground                         |
| 6   | RXEN       | free GPIO (strapping) | **12**     | 10k pull-down REQUIRED (R2)     |
| 7   | TXEN       | LED pin (repurposed)  | **25**     | 10k pull-down (R1)             |
| 8   | DIO2       | leave unconnected     | —          | "hang it" per datasheet         |
| 9   | VCC        | 3.3 V rail (LM2596)   | —          | C1+C2+C3 across VCC→GND here    |
| 10  | VCC        | 3.3 V rail            | —          | **tie to pad 9** (don't float)  |
| 11–12| GND       | GND                   | —          | ground                         |
| 13  | DIO1       | free input-only GPIO  | **39**     | input-only pin OK (IRQ in)      |
| 14  | BUSY       | free input-only GPIO  | **36**     | input-only pin OK               |
| 15  | NRST       | free GPIO             | **33**     | optional 10k pull-up (R3)       |
| 16  | MISO       | VSPI MISO (shared)    | **19**     | shared w/ onboard LoRa + SD     |
| 17  | MOSI       | VSPI MOSI (shared)    | **27**     | shared                         |
| 18  | SCK        | VSPI SCK (shared)     | **5**      | shared                         |
| 19  | NSS (CS)   | free GPIO             | **32**     | E22's own chip-select          |
| 20  | GND        | GND                   | —          | ground                         |
| 21  | ANT        | 433 MHz λ/4 whip      | —          | 50 Ω; OR use IPEX connector     |
| 22  | GND        | GND                   | —          | ground                         |

Onboard SX1276 (CS=GPIO18) and microSD (CS=GPIO23) stay on the same bus — three
CS lines (18/23/32), firmware selects only one device at a time. Leave the
onboard LoRa deselected; the E22 is your radio.

## Passives to add (BOM)

| Ref | Value        | Type                          | Placement                              |
|-----|--------------|-------------------------------|----------------------------------------|
| C1  | 470–1000 µF  | electrolytic/tantalum, low-ESR| VCC → GND, within a few mm of E22 VCC   |
| C2  | 10 µF        | ceramic X7R/X5R, ≥16 V        | VCC → GND, next to C1                    |
| C3  | 100 nF       | ceramic X7R                   | VCC → GND, closest pad to the VCC pin   |
| R1  | 10 kΩ        | resistor                      | TXEN (GPIO25) → GND *(keeps PA off @boot)* |
| R2  | 10 kΩ        | resistor                      | RXEN (GPIO12) → GND  *(required)*        |
| R3  | 10 kΩ        | resistor (optional)           | NRST (GPIO33) → 3.3 V                    |

C1 handles the 1 W burst (≈0.6–1 A), C2 the mid-frequency, C3 the high-frequency
switching noise. Together they stop the VCC rail from dipping and browning out
the ESP32 / resetting sensors mid-transmit.

## PCB layout notes (when you update the board)

- Place **C1 within a few mm of the E22 VCC pin**; short, wide VCC + GND traces
  (or a small pour) from the LM2596 rail to the module. This is the whole point
  of the caps — long thin VCC traces re-introduce the voltage drop.
- Solid ground under the module; star-ground the cap returns to the buck output.
- **Separate antenna for the E22**: 50 Ω feed, U.FL/SMA → a 433 MHz λ/4 whip
  (~16.5 cm). The Heltec's U.FL belongs to the *internal* SX1276, not the E22.
- Keep the antenna feed short and away from the LM2596 switching nodes.

## Firmware (RadioLib, SX1268)

```cpp
#include <RadioLib.h>
// SX1262/68 module: NSS, DIO1, NRST, BUSY
SX1268 radio = new Module(32, 39, 33, 36);   // NSS=32, DIO1=39, NRST=33, BUSY=36
// TXEN=25, RXEN=12  -> tell RadioLib to drive the PA/LNA RF switch:
void setup() {
  SPI.begin(5, 19, 27, 32);                  // SCK=5, MISO=19, MOSI=27, CS=32
  radio.begin(433.0);                        // 433 MHz
  radio.setRfSwitchPins(12 /*RXEN*/, 25 /*TXEN*/);
  radio.setSpreadingFactor(11);              // SF11 for penetration/range
  radio.setBandwidth(125.0);
  radio.setCodingRate(8);                    // 4/8 max FEC
  radio.setOutputPower(22);                  // SX1268 sets its PA; E22 PA gives ~1 W
}
```

Leave the onboard SX1276 uninitialised (or init on CS=18 as a backup logger, but
it's the same ESP32 + battery, so not true redundancy).

## Implement in Eeschema (safe, DRC-checked)

1. Create/import an `E22-400M30S` symbol (11 pins: VCC, GND, SCK, MOSI, MISO,
   NSS, NRST, BUSY, DIO1, TXEN, RXEN) with pad numbers per your module datasheet.
2. Place it + C1/C2/C3 + R1/R2 (R3 optional).
3. Wire per the tables above; label the shared SPI nets so they merge with the
   existing SCK/MOSI/MISO nets on U1.
4. Run **Inspect → Electrical Rules Check**; fix any unconnected-pin warnings.
5. **Tools → Update PCB from Schematic** to pull the new footprints in.
