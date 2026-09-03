# ELE3km — dropping the E22, single-radio (SX1276-only) design

**Status:** decided 2026-08-30. Design work is for the next session; this file
records the decision, why, and the questions that redesign must resolve. Nothing
in the firmware has been changed yet beyond restoring the flight build.

**Decision:** remove the E22 (433 MHz, SX1268) entirely. All telemetry goes over
the onboard Heltec SX1276 (915 MHz) only.

## Why

Bench bring-up of the E22 failed and the root causes were traced on the bench:

- **Broken MOSI trace** from the shared VSPI net to E22 pad 17 — the real reason
  `RadioE22::begin()` returned `CHIP_NOT_FOUND`. The chip never received a
  command, so it never answered on MISO. Continuity to pad 17 was open; MISO,
  SCK, NSS, VCC, GND all verified good.
- **U6 (LM2596) rail reads 3.4 V**, above the 3.30 V the hardware doc mandates.
  An LM2596 trimmed high overshoots on power-on; repeated turn-on spikes are the
  likely cause of the parts that died on the U6 rail. The E22 and the original
  BMP280 are both on U6 and both failed; the SX1276 is on U7 (Heltec internal
  3V3) and never had trouble. That rail split is the clean tell.

Given the E22 branch is unreliable on this board revision and the added
complexity (second SPI device, 1 W PA rail contention, boot-loop/brownout
mitigation) buys a second radio we could not bring up, the call is to ship a
single-radio firmware and revisit a 433 MHz link only if a board respin fixes the
rail and the trace.

## What "SX1276-only" removes or changes

By area, so the next session can scope the edit. These are impacts, not yet
decisions.

- **HAL:** delete `hal/radio_e22.*` from the build and from `main.cpp` init/tasks.
  The SX1276 HAL (`hal/radio_sx1276.*`) stays and becomes the sole radio.
- **Scheduler (`core/tx_scheduler.h`):** the two exclusion rules exist mostly for
  the E22. Per `requisitos_v2` R4, the "no SD write during TX" rule protects the
  **U6 rail from the E22's ~1 A PA burst** — the SX1276 is not on that rail, so
  that rule can go. Re-derive what, if anything, the scheduler still needs.
- **Boot-loop / survival mode (issue 12):** this mitigation exists largely because
  the E22's 1 W PA sags the rail and causes brownout resets. With the E22 gone,
  the primary brownout source is gone. Decide whether survival mode is still
  warranted (the SX1276 SF12 post-landing beacon can stay regardless).
- **Packet contract (`PACKET_FORMAT.md`):** the 20 B / 12 B formats stay. What
  changes is the note that "two radios send the same packet ~500 ms apart, dedup
  by sequence" — there is now one radio. The ground station's cross-radio dedup
  simplifies to a single-stream wrap-handling.
- **Pins (`include/pins.h`):** frees GPIO32 (NSS), 33 (NRST), 36 (BUSY), 39
  (DIO1), 25 (TXEN), 12 (RXEN). Note 12 is a strapping pin and 25/12 have the R1/R2
  pulldowns; leaving them unused is fine.
- **Ground station:** already the 915 MHz SX1276 receiver we debugged this session.
  Drop any assumption of a second (433 MHz) stream; keep the AFC work (below).

## The one question the redesign must answer first — link budget

The E22 was the long-range link with a **documented** budget: ~103 dB free-space
loss at 8 km / 433 MHz against +30 dBm and a ground Yagi, tens of dB of margin
(`ELE3km_hardware_constraints.md` §link budget). Removing it leaves only the
SX1276 at **915 MHz, +20 dBm (100 mW), SF7 in flight** — for which no 3 km link
budget is written down.

**Does the SX1276 link alone close 3 km?** This must be computed and, ideally,
range-tested before committing. Levers if the margin is thin: raise the flight
spreading factor (SF7 → higher, costs airtime and telemetry rate), a directional
/ higher-gain ground antenna, and confirming the +20 dBm regulatory headroom in
the 915–928 MHz band. Do not assume it closes — the 433/1 W link was the easy one
by design, and this removes it.

## Also decide next session

- Redundancy is now single-rail (U7) and single-band. Accept the single point of
  failure, or plan a board respin that keeps a second radio on a clean rail.
- Survival beacon: keep SX1276 SF12 post-landing? (Recommended yes; it is the
  recovery link.)
- Whether to raise flight SF on the SX1276 for margin, and re-run the airtime /
  duty-cycle budget if so.

## Hardware, independent of the firmware decision

- **Fix U6 to 3.30 V** (trim the LM2596 with no modules loaded, then reconnect).
  This matters regardless of the E22 — it is the suspected killer of the BMP280
  and the E22s, and the sensors + SD still live on U6.
- The broken MOSI trace to E22 pad 17 can be left as-is if the E22 is abandoned;
  it is on the shared VSPI net but an open stub does not affect the SX1276 or SD.

## Related files

- `Docs/E22_integration.md` — now on hold (banner added). Kept for the record and
  for a possible future respin.
- `Docs/ELE3km_hardware_constraints.md` — link budget and rail (U6/U7) split.
- `Docs/ELE3km_firmware_PRD_v3.md` — the firmware requirements to amend.
- `ELE3km/PACKET_FORMAT.md` — dual-radio dedup note to revise.
