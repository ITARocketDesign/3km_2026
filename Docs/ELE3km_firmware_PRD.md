# PRD — ELE3km Flight-Computer Firmware

**Project:** ELE3km rocket telemetry flight computer
**Target:** Heltec WiFi LoRa 32 **V2** (ESP32) as the flight computer; **Ebyte E22-400M30S** (SX1268, 433 MHz, 1 W) as the sole transmitter
**Build system:** PlatformIO — hardware behind a thin HAL, flight logic in a pure host-testable `core/`, native unit tests
**Status:** Ready for implementation

---

## Problem Statement

The team is flying a rocket (800–1000 km/h, 3–8 km line-of-sight) in a Brazilian rocketry competition and needs to (a) recover a complete flight-data record after the flight and (b) keep receiving the rocket's live position and altitude during ascent, descent, and — critically — after it lands in forest ~500 m from the clearing, so they can walk to it. Today there is no firmware: the board is wired and reviewed, but nothing reads the sensors, logs them, or transmits anything. Without firmware the board is inert; without a robust telemetry/logging strategy the team loses either the science data or the rocket itself.

## Solution

Firmware that turns the ELE3km board into a flight computer. It continuously reads the **accelerometer/gyro (MPU6050)**, **barometer (BMP280)**, and **GPS (NEO-6M)**, logs everything to the **microSD** card as a durable flight record, and transmits a compact live telemetry stream over the **E22** radio. The ESP32's own onboard SX1276 radio is **never used to transmit** — the ESP32 is only the computer; the E22 is the only thing that emits RF.

The transmitted telemetry carries the rocket's **position and altitude**. Position is derived from the GPS, with an **accelerometer-based inertial (INS) dead-reckoning fallback** when the GPS fix is lost, and a **Kalman filter** fuses the sources to refine the estimate used for position. **Altitude is the priority payload**: whenever a full position-plus-altitude packet cannot be sent (position estimate not trustworthy, or link/airtime too tight), the firmware falls back to transmitting **altitude only**, so the ground station always gets at least the altitude.

## User Stories

1. As a recovery operator, I want the rocket to keep transmitting its position after it lands, so that I can walk to it in the forest instead of losing it.
2. As a recovery operator, I want at minimum the altitude to always come through even when position doesn't, so that I never get total telemetry silence.
3. As a ground-station operator, I want live position and altitude during flight, so that I can track the rocket through ascent and descent.
4. As a ground-station operator, I want each transmitted packet to indicate whether the position came from GPS or from the inertial fallback, so that I know how much to trust it.
5. As a ground-station operator, I want a sequence number and timestamp in each packet, so that I can detect dropped packets and order them.
6. As a flight-data analyst, I want every raw sensor reading (accel, gyro, baro pressure/temp, GPS) logged to the microSD card with timestamps, so that I can reconstruct the whole flight afterward.
7. As a flight-data analyst, I want the fused estimator outputs (position, altitude, vertical velocity, position source) logged alongside the raw data, so that I can evaluate how the Kalman filter performed.
8. As a flight-data analyst, I want a mid-flight power loss or crash to cost me only the tail of the log rather than the whole file, so that I still have a usable record. *(Achieved by the pre-allocated contiguous file — **not** by flushing frequently, which would put a FAT write in the path of every TX burst.)*
9. As the flight computer, I want to read the MPU6050 accelerometer and gyro at a high rate, so that inertial dead-reckoning and the Kalman predict step have fresh motion data.
10. As the flight computer, I want to read the BMP280 barometer, so that I have a reliable primary altitude source independent of GPS.
11. As the flight computer, I want to parse the NEO-6M GPS over UART, so that I have absolute position and a GPS altitude fallback.
12. As the flight computer, I want to run a Kalman filter that fuses GPS position with integrated accelerometer motion, so that the position I transmit is smoother and more robust than raw GPS.
13. As the flight computer, when the GPS fix is valid I want the Kalman filter to correct position and velocity from GPS, so that inertial drift is continuously reset.
14. As the flight computer, when the GPS fix is lost or stale I want to propagate position by integrating accelerometer data (INS fallback), so that I still transmit a position estimate during the outage.
15. As the flight computer, when the GPS fix returns I want the estimate to re-converge to GPS, so that transient inertial drift is corrected.
16. As the flight computer, I want to compute altitude primarily from the barometer (fused with vertical acceleration), so that altitude stays accurate even without GPS.
17. As the flight computer, I want to transmit a full packet containing position + altitude whenever the position estimate is trustworthy, so that the ground gets complete telemetry.
18. As the flight computer, I want to transmit an altitude-only packet whenever a full packet can't be sent, so that altitude always gets through.
19. As the flight computer, I want the E22 to be my only transmitter, so that all RF energy and the antenna are dedicated to the long-range link.
20. As the flight computer, I want the onboard ESP32 SX1276 radio to remain silent and never transmit, so that it neither interferes nor wastes the shared SPI bus / power.
21. As the flight computer, I want to hold the E22's TXEN/RXEN lines safe at boot and let the radio driver own them, so that the 1 W PA is never keyed accidentally.
22. As the flight computer, I want to share the SPI bus safely between the microSD and the E22 by selecting one device at a time, so that neither corrupts the other's transfers.
23. As the flight computer, I want to avoid writing to the microSD during an E22 transmit burst where practical, so that the 1 W TX current draw on the shared 3.3 V rail doesn't corrupt an SD write.
24. As the flight computer, if the IMU is absent or not responding, I want to disable the inertial fallback and keep running on GPS + barometer, so that a missing/faulty IMU doesn't take down the whole system.
25. As the flight computer, if the GPS has no fix yet, I want to keep logging and transmitting altitude, so that the system is useful before GPS lock.
26. As the flight computer, if the barometer is absent, I want to fall back to GPS altitude, so that altitude telemetry survives a baro failure.
27. As the flight computer, if the microSD fails or is absent, I want to keep transmitting telemetry, so that a logging failure never stops the recovery link.
28. As the flight computer, I want to detect each sensor at startup (I²C WHO_AM_I / GPS traffic / SD mount / E22 handshake), so that I log which subsystems are healthy.
29. As the flight computer, I want the telemetry transmit cadence tuned to the LoRa airtime at the chosen spreading factor, so that I stay within a sane duty cycle and don't queue-starve.
30. As the flight computer, I want to keep transmitting at a slow, robust cadence once landing is confirmed (see story 37 for the confirmation criteria — *not* "low motion"), so that the post-landing beacon maximizes the chance of being heard.
31. As a firmware developer, I want the Kalman filter, the GPS→INS fallback logic, and the packet codec to live in a pure core with no hardware dependencies, so that I can unit-test them on my laptop.
32. As a firmware developer, I want to feed synthetic sensor sequences (including GPS dropouts and noise) into the flight-computer core and assert on the emitted log records and packets, so that I can verify behavior without flying.
33. As a firmware developer, I want the hardware drivers isolated behind a thin HAL, so that swapping a sensor or the radio doesn't ripple into the flight logic.
34. As a firmware developer, I want the RF-switch pin mapping (RXEN=GPIO12, TXEN=GPIO25) and all bus pins defined in one place matching the real netlist, so that firmware and hardware never disagree.
35. As a firmware developer, I want a documented telemetry packet format (full and altitude-only), so that the ground-station receiver firmware can decode it.
36. As a ground-station operator, I want live telemetry at ~1 Hz throughout the entire flight — pad, boost, coast, descent and landing — not only after touchdown, so that I can actually track the vehicle.
37. As a recovery operator, I want landing detection to require ~1 g plus stable altitude near ground level, so that free fall at apogee is never mistaken for landing and the descent telemetry is not lost.
38. As a flight-data analyst, I want the ground station to log every received packet raw to disk, so that I still have a flight record if the rocket or its SD card is never recovered.
39. As a flight-data analyst, I want the log written into a pre-allocated contiguous file with no FAT updates during flight, so that a power loss costs me the tail of the log rather than the whole file.
40. As the flight computer, I want the barometric ground reference persisted and reused after an unexpected reset, so that a reboot at altitude does not make me report 0 m.
41. As the flight computer, I want IMU samples that hit the ±16 g / ±2000 °/s limits flagged as saturated, so that clipped boost data is not integrated as if it were valid.

## Implementation Decisions

### Modules

- **`core/` (pure, no hardware, host-testable):**
  - **Estimator** — a Kalman filter maintaining horizontal position + velocity and a vertical (altitude + vertical-velocity) channel. Predict step driven by IMU acceleration; update step from GPS (horizontal + GPS altitude) and from the barometer (vertical). Exposes current estimate, per-channel covariance/confidence, and the active **position source** (GPS vs INS).
  - **Position-source state machine** — GPS-valid → source = GPS; GPS lost/stale for a configurable number of samples/time → source = INS (predict-only, dead reckoning); GPS re-acquired → source = GPS and estimate re-converges. Emits a confidence flag used to decide full vs altitude-only packets.
  - **Telemetry codec** — serializes two packet shapes: **full** (sequence, time, latitude, longitude, altitude, position-source/fix-quality) and **altitude-only** (sequence, time, altitude, source flag). Fixed, documented, endian-defined binary layout so the ground receiver can decode it. Altitude is present in *both* shapes.
  - **FlightComputer** — the orchestrator: `update(SensorSample) → { LogRecord, optional<TelemetryPacket> }`. Owns the estimator + state machine, decides when to emit a packet (cadence) and which shape (full vs altitude-only), and always produces a log record. This is the primary test seam.
- **`hal/` (thin adapters, on-target):** MPU6050 (I²C), BMP280 (I²C), NEO-6M (UART, NMEA/UBX parse), microSD (SPI, FAT), **E22/SX1268 radio (SPI, via RadioLib)**.
- **`src/main.cpp`:** wires HAL to the core — sample the sensors into a `SensorSample`, call `FlightComputer::update`, write the returned `LogRecord` to SD, and hand any returned `TelemetryPacket` to the E22 driver.

### Estimator / position

- **GPS is the primary position source.** The Kalman filter fuses it with integrated accelerometer motion; the GPS update step continually resets inertial drift.
- **INS fallback** is dead reckoning from the accelerometer during GPS outages — explicitly a *short-term bridge*, not precision navigation (the MPU6050 is 6-axis, no magnetometer; double-integrated horizontal position drifts and heading is unobservable). Its job is to keep transmitting *a* position and to bridge brief dropouts, not to navigate for minutes.
- **Altitude is computed primarily from the barometer**, fused with vertical acceleration for dynamic response; it does **not** depend on GPS and is therefore the most reliable payload. GPS altitude is only a fallback if the barometer is unavailable.

### Telemetry / radio

- **The E22 (SX1268) is the only transmitter.** Common config: 433 MHz, BW 125 kHz, CR 4/8, CRC on, explicit header, output power set so the E22 PA delivers ~1 W. RF switch driven by RadioLib with **RXEN = GPIO12, TXEN = GPIO25** (per the real netlist — note this differs from the older integration doc that said GPIO2).
- **Spreading factor is switched by flight phase, not fixed.** *(Supersedes the earlier SF11 baseline — see `ELE3km_hardware_constraints.md` §C1 for the derivation.)*

  | Phase | SF | Cadence | Airtime | TX duty |
  | ----- | -- | ------- | ------: | ------: |
  | **FLIGHT** (pad → landing) | SF8 | 1 Hz | 140 ms | 14 % |
  | **LANDED** | SF12 | 1 / 20 s | 1712 ms | 8.6 % |

  A 20-byte packet at SF11 costs **987 ms** of airtime, during which the PA draws ≈1 A from the shared 3.3 V rail. SF11 therefore cannot sustain continuous in-flight telemetry — it caps out near one packet per 8–10 s. SF8 costs 140 ms, so **1 Hz for the whole flight** fits inside a 14 % duty cycle. The in-flight link is line-of-sight (≈103 dB free-space path loss at 8 km / 433 MHz) and SF8 keeps tens of dB of margin against +30 dBm and a ground Yagi. SF12's extra sensitivity is only worth its airtime **after landing**, where the rocket lies under foliage and cadence no longer matters.
- **A checked duty-cycle ceiling** bounds transmissions over a rolling window, and the cadence logic may not exceed it whatever the flight phase requests. This protects the PA thermally and the battery, and it must be a verified constant, not a comment.
- **WiFi and Bluetooth are explicitly disabled.** Neither is used; both cost 100–250 mA in bursts and add CPU interrupts.
- **The onboard SX1276 is never initialized for transmit.** It stays deselected on the shared SPI bus; the ESP32 acts purely as the computer.
- **Altitude-priority policy:** attempt a **full** packet when the position estimate is trustworthy (GPS fix, or INS still within confidence); otherwise send an **altitude-only** packet. Altitude is the guaranteed minimum payload in every transmission.
- **Post-landing beacon mode:** on landing, switch to SF12 at a slow cadence to maximize recovery odds. **Landing detection must require all four of:** (1) acceleration magnitude near **1 g** — *not merely stable*; (2) barometric altitude stable within a band for N consecutive seconds; (3) altitude near the stored ground reference; (4) a minimum time elapsed since liftoff. **In free fall the accelerometer reads ≈0 g steadily, so a low-variance "low motion" detector fires at apogee** and would drop the vehicle into beacon mode for the entire descent — destroying the in-flight telemetry this PRD exists to provide. The transition is one-way: only a reset leaves LANDED. Flight phase appears in every log record and every packet.

### Bus / power arbitration

- microSD and E22 share VSPI (plus the silent SX1276). Firmware serializes access by chip-select; only one device active per transfer. GPIO18 (onboard SX1276 NSS) floats on this board and **must be driven HIGH before `SPI.begin()`**, or the onboard radio drives MISO against both other devices.
- **The scarce resource is the 3.3 V rail, not the bus.** The payload is written to the E22 before TX is keyed; during airtime the radio transmits autonomously and SPI is idle. What cannot overlap is the ≈1 A PA draw and an SD write on the same rail.
- **A three-state cycle — `RADIO_TX` → `SD_WRITE` → `IDLE` — with a RAM ring buffer.** No SD write may start while TX is keyed (a hard rule, not best-effort); a TX request never preempts an in-flight SD write. States are events of variable duration, not fixed time slices, so a card stall degrades log resolution instead of breaking the cycle. Sensor sampling runs continuously across all three states (I²C and UART only) — the IMU cannot be paused for an airtime without breaking the Kalman predict step. On buffer overflow, drop the oldest log records; never delay a sensor read or a packet. Full design in `ELE3km_hardware_constraints.md` §C3.

### Pin / bus contract (from the netlist — single source of truth in firmware config)

| Function                     | ESP32 pin                         |
| ---------------------------- | --------------------------------- |
| VSPI SCK / MOSI / MISO       | GPIO5 / GPIO27 / GPIO19           |
| microSD CS                   | GPIO23                            |
| E22 NSS / NRST / BUSY / DIO1 | GPIO32 / GPIO33 / GPIO36 / GPIO39 |
| E22 RXEN / TXEN (RF switch)  | GPIO12 / GPIO25                   |
| I²C SDA / SCL (IMU + baro)   | GPIO21 / GPIO22                   |
| GPS TX→ / ←RX (UART)         | GPIO13 / GPIO17                   |

### Logging

- Every cycle produces a timestamped log record containing raw sensor values (accel xyz, gyro xyz, baro pressure/temp, GPS lat/lon/alt/fix/sats) **and** fused outputs (position, altitude, vertical velocity, position source, flight phase, transmitted sequence number). Records are **fixed-size 64-byte binary**, each carrying a magic marker, sequence number and CRC, so a post-flight tool can recover every valid record by scanning the raw file.
- **The log file is pre-allocated and contiguous, and the FAT is not touched during flight.** Pre-allocate at boot via `SdFat::preAllocate()`, then write only raw 512-byte aligned data blocks into it; metadata is written exactly twice, at creation and at close. *This replaces "flush frequently"* — a flush is itself a FAT write and is the single most dangerous operation to have coincide with a TX burst. The failure mode being designed out is an interrupted FAT or directory-entry write, which destroys the **entire** log rather than one record.
- **Use SdFat, not the Arduino `SD` library** — `SD` supports neither pre-allocation nor contiguous files, which are the whole mechanism above.
- A header block at offset 0 carries magic, format version, boot counter and the ground reference pressure. **A new file is created per boot** (incrementing index) so a mid-flight reset never clobbers the previous one. Timestamps are taken at acquisition, never at write time.
- **The ground station is the second copy of the flight data.** It must append every received packet, raw, with a reception timestamp, to disk. This costs one line in the receiver and yields a flight record that survives loss of the SD card *or* of the vehicle itself — the packet format should be sized with that backup role in mind, not only as a tracking aid.

### Graceful degradation

- Each subsystem is independently optional and detected at startup. Missing IMU → INS fallback disabled, run on GPS + baro. No GPS fix → altitude + INS still transmit. Missing baro → GPS altitude. SD failure → telemetry continues. A single failure never silences the recovery link.

## Testing Decisions

- **What makes a good test:** exercises *external behavior* of the core, not its internals. Given an input stream of `SensorSample`s, assert on the emitted `LogRecord`s and `TelemetryPacket`s — never on private filter matrices or intermediate variables.
- **Primary seam — FlightComputer:** feed synthetic sample sequences and assert:
  - Altitude appears in **every** emitted packet.
  - A **full** packet is emitted while GPS is valid; an **altitude-only** packet is emitted once position confidence drops (GPS dropout beyond the fallback window).
  - Packet sequence numbers increment and cadence matches the configured rate.
  - With the IMU marked absent, no INS fallback is attempted and GPS+baro telemetry still flows.
- **Flight-phase state machine — the highest-value test in the suite:** feed a synthetic full-flight profile (pad → boost → coast → **apogee free fall** → descent → touchdown) and assert that the phase stays FLIGHT through apogee. A free-fall segment reads ≈0 g *steadily*, so this test is what catches a landing detector built on low variance instead of on ≈1 g. Also assert: LANDED is entered only when all four conditions hold, LANDED is never left, and packets keep flowing at the FLIGHT cadence for the whole descent.
- **Transmit cadence and duty ceiling:** assert the emitted packet rate matches the phase config, and that the duty-cycle ceiling is never exceeded even when the cadence logic asks for more.
- **Bus arbitration:** with a fake radio and fake card, assert no SD write is ever issued while TX is keyed, that a card stall spanning several cycles drops the oldest log records rather than delaying a packet, and that a TX request waits for an in-flight write instead of preempting it.
- **Log codec / recovery:** assert a raw byte stream truncated at an arbitrary offset still yields every complete record when scanned, and that CRC-corrupted records are rejected rather than silently accepted.
- **Saturation handling:** feed IMU samples clipped at full scale and assert they are flagged and excluded from (or downweighted in) the predict step.
- **Baro reference persistence:** simulate a reset mid-flight and assert the stored ground reference is reused rather than re-zeroed, so reported altitude stays continuous across the reboot.
- **Estimator:** drive it with a known synthetic trajectory plus noise; assert the estimate tracks within a bounded error, that the GPS update reduces error, and that during a GPS gap it runs predict-only and position uncertainty grows.
- **Position-source state machine:** assert GPS-valid → source = GPS; N stale GPS samples → source = INS; GPS return → source = GPS and re-convergence.
- **Telemetry codec:** encode→decode round-trips for both packet shapes; altitude decodes correctly from the altitude-only packet; encoded sizes stay within the LoRa payload budget.
- **Prior art:** none — this is greenfield. This PRD establishes the pattern: pure `core/` under `test/native/`, HAL excluded from unit tests.
- **HAL adapters:** verified on-target (flash + observe), not in native unit tests.

## Out of Scope

- **Ground-station receiver firmware** (the decoding/display side) — separate effort; this PRD defines the packet format it must consume. **One requirement is imposed on it and is not optional:** it must append every received packet, raw and timestamped, to disk (see Logging). That log is the project's insurance against losing the vehicle, so it is a dependency of this PRD rather than out of scope.
- **The onboard SX1276 as an active radio** — it stays silent by decision.
- **OLED display** — physically unavailable on this board (GPIO21 repurposed as I²C SDA).
- **Precision / long-duration inertial navigation** — the 6-axis IMU cannot support it; INS is a short-term bridge only.
- **Recovery-event actuation** (parachute/pyro deployment) — not part of this board's job.
- **Hardware fixes** — e.g., the IMU GND external jumper documented in `ELE3km_connections.md` is an assembly action; firmware only *detects and degrades* if the IMU is dead, it cannot fix the wiring.
- **Regulatory/frequency selection beyond 433 MHz** — band is fixed by the E22-400M30S hardware.

## Further Notes

- **Honest limitation on the INS fallback:** with a magnetometer-less 6-axis IMU, dead-reckoned horizontal position degrades quickly (seconds to low tens of seconds of useful bridging). This is acceptable because (1) altitude — the priority payload — is barometric and unaffected, and (2) the fallback's purpose is continuity during brief GPS dropouts, not autonomous navigation. This should be stated to the ground crew so they trust GPS-sourced fixes over INS-sourced ones (hence the source flag in every packet).
- **Rail-sag interaction:** the E22's 1 W TX bursts share the 3.3 V rail with the sensors and SD (see `ELE3km_connections.md`); the SD-write/TX-burst scheduling mitigation above exists specifically to protect log integrity.
- **Firmware/hardware single source of truth:** the pin map above is taken from the exported netlist; if the board is revised, update it in exactly one firmware config location.
- **Estimator caveats forced by the hardware** (detailed in `ELE3km_hardware_constraints.md`): the MPU6050 saturates at ±16 g and ±2000 °/s and **will clip during boost** — clipped samples must be flagged and not integrated as valid, or velocity is systematically underestimated. The barometer is unreliable through the transonic regime, so it must be downweighted when vertical acceleration or velocity is high, despite being the primary altitude source everywhere else. Sensors share the rail with the 1 W PA and can reset silently, so configuration registers (not just data registers) must be re-verified periodically.
- **Cross-references:** `ELE3km_connections.md` (netlist-accurate wiring + the IMU GND jumper action), `ELE3km_hardware_constraints.md` (**the hazard analysis and the chosen radio/SD/arbitration design — read this before writing code**), `E22_integration.md` (radio wiring — note its `setRfSwitchPins(2, 25)` snippet is stale; RXEN is GPIO12, so it should be `setRfSwitchPins(12, 25)`).
