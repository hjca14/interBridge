# Si3050/Si3011-19 Firmware Foundation (Phase 3A)

This document describes the Phase 3A firmware foundation for the Si3050
DAA (+ Si3011/18/19 line-side device) that will interface the ESP32-C3 to
the analog intercom line on hardware Rev A. It is a **testable
foundation, not a working driver**: it has been validated with mocks and
compilation only, on a host machine, with no Rev A board in existence yet.
Nothing here has been run against real hardware.

## Scope of this PR

Implemented:

- The hardware-independent Si3050 electrical **bring-up sequence**
  (`Si3050Controller`), gating SPI access on it having completed.
- Narrow HAL interfaces for the four things that sequence needs (SPI bus +
  chip select, PCM clock, `/RESET`, and a short injectable delay), each
  with a real ESP32-C3 stub and a deterministic fake for tests.
- A compile-time-checked Rev A pin map (`si3050_pins.h`).
- A debounced `/RGDT` ring-signal-line reader (`RingDetector`), reporting
  sanitized `Asserted`/`Cleared` level-change events only.
- Native tests for all of the above (see "Tests" below).

Explicitly **not** implemented (deferred to future, bench-validated PRs):

- Any Si3050/Si3011-19 **control register** read or write (DAA/line
  parameters, PCM highway mode enable, powering up the line-side device,
  etc. - steps 3-6 of the datasheet's documented initialization
  procedure, as numbered in the checked-in `docs/Si3050-11-18-19.pdf`
  Rev. 1.5 copy; step numbering, like section numbering, is not assumed
  stable across other datasheet revisions).
  `Si3050Controller::initialize()` stops right where the datasheet's
  electrical pin-timing requirements end and register-level
  configuration begins.
- Real PCM clock generation (`Esp32PcmClock`): a stable, phase-locked
  2.048 MHz PCLK with a correctly framed 8 kHz FSYNC needs a timer/I2S
  peripheral configuration that has not been verified on real ESP32-C3
  hardware in this repository - `isRunning()` always reports `false` so
  nothing downstream can mistake this for a working clock.
- Real SPI transactions (`Esp32Si3050Bus::transfer()`): the SPI clock
  polarity/phase the Si3050 expects has not been confirmed against real
  hardware here, so it remains an explicit `TODO` rather than a guess.
- Real ring **pattern** validation, off-hook detection, line
  characterization, or audio. `RingDetector` reports a debounced
  electrical level change only - see "Ring detection" below for exactly
  why that is not the same as a validated ring.
- Anything touching `IHardwareIO`, `setDoorOutput()`, or any door
  actuation path. The new module has no dependency on any of that by
  construction (verified by grep as part of this PR's validation, not
  just by inspection of the two files that could plausibly need it).

## Hardware source of truth

Pin numbers below match
https://github.com/hjca14/interhardware/blob/main/docs/decisions/INTERBRIDGE_GPIO_MAP.md
(Rev A). `si3050_pins.h` is the single place those numbers live in this
firmware, with `static_assert`s that they never collide with the
ESP32-C3's own reserved pins:

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 0 | PCLK (PCM clock) | ESP -> Si3050 | |
| 1 | FSYNC (PCM frame sync) | ESP -> Si3050 | |
| 2 | SDO / SPI MISO | Si3050 -> ESP | external 10 k pull-up to 3.3 V |
| 3 | DTX (PCM data) | Si3050 -> ESP | |
| 4 | DRX (PCM data) | ESP -> Si3050 | |
| 5 | `/RESET` | ESP -> Si3050 | active low, external pull-down |
| 6 | SCLK (SPI clock) | ESP -> Si3050 | sampled at RESET to select PCM/SPI vs. GCI mode |
| 7 | SDI / SPI MOSI | ESP -> Si3050 | |
| 8 | `/RGDT` (ring detect) | Si3050 -> ESP | open-drain, active low, external 4.7 k pull-up |
| 9 | BOOT | reserved | ESP32-C3 download-mode strap - never repurposed |
| 10 | CS (SPI chip select) | ESP -> Si3050 | active low |
| 18/19 | USB D-/D+ | native | |
| 20 | Button | input | existing config/reset button |
| 21 | Status LED | output | existing WS2812B |

`AOUT/INT` (Si3050 pin 7) is **not** wired to the ESP in this Rev A - it
stays on a bare bench pad. Nothing in this firmware assumes a use for it.

## Datasheet reference

`docs/Si3050-11-18-19.pdf` (Skyworks "Si3050+Si3011/18/19", Rev. 1.5,
August 24 2021) is checked into this repository and is the source for
every timing value used below - none of it is guessed. Cited by section
title below, not section number (numbering varies between datasheet
revisions - see docs/si3050-clock-probe.md's "Corrected premise" for
why this matters). Relevant sections:

- **Table 6, "Switching Characteristics - General Inputs" (page 10):**
  `PCLK Before RESET` (t<sub>mr</sub>) &ge; 10 cycles; `CS, SCLK Before
  RESET` (t<sub>mxr</sub>) &ge; 20 ns; `RESET Pulse Width` (t<sub>rl</sub>)
  &ge; the greater of 250 ns or 10 PCLK cycle times.
- **"Clock Generation" (page 39):** PLL settle time `Tsettle = 64 /
  FPCLK`. At the Rev A target of 2.048 MHz, this is 31.25 &micro;s
  (rounded up to 32 &micro;s in code). The datasheet separately states
  this is "less than 1 ms from the application of PCLK", consistent with
  the formula at any valid PCLK rate. Note: 2.048 MHz is a target chosen
  for Rev A, not the only valid PCLK rate - PCM/SPI mode (the mode
  InterBridge plans to use, see below) also accepts 1.024 MHz; see
  docs/si3050-clock-probe.md's "Corrected premise" for the full
  distinction from GCI mode's stricter 2.048/4.096 MHz requirement.
- **"Communication Interface Mode Selection" / Table 20 (page 39):** the
  state of SCLK at the instant RESET is sampled selects PCM/SPI mode
  (SCLK=1) vs. GCI mode - why SCLK must be held high before RESET is
  released.
- **Pin description table:** `/CS`, `/RESET`, and `/RGDT` are each
  explicitly documented as active-low.
- **"Ring Detection" (page 33):** `/RGDT` is open-drain, defaults active
  low, and toggles *at the ring signal's own frequency* during an actual
  ring burst - it is not a clean single asserted/cleared level. See
  "Ring detection" below.

## Electrical bring-up contract

`Si3050Controller::initialize()` (in `src/intercom/si3050/
si3050_controller.cpp`) returns a `Si3050InitResult` (`Ready`,
`InvalidConfig`, or `ClockNotRunning`) and performs exactly these steps,
in order, never touching a control register - but only past two
fail-closed gates:

0. **Config gate.** `Si3050Config` is validated first: `pclkHz != 0 &&
   fsyncHz != 0`. An invalid config never touches the bus or clock and
   never runs any timing math (which would otherwise divide by `pclkHz`);
   it actively (re)asserts `/RESET` and returns `InvalidConfig`.
1. CS deselected (high).
2. `/RESET` asserted (low).
3. SCLK held high (selects PCM/SPI mode - sampled by the Si3050 when
   RESET is later released).
4. PCLK/FSYNC started via `clock.start()`. **Clock gate.** The result is
   not trusted blindly: `clock.isRunning()` is checked immediately after.
   If it reports `false`, bring-up stops here - `/RESET` stays asserted,
   neither wait below runs, `isReady()` stays `false`, and this call
   returns `ClockNotRunning`. Because `Esp32PcmClock::isRunning()` always
   reports `false` (real PCM clock generation is not implemented - see
   above), this means the controller structurally refuses to finish
   bring-up against that stub, so a future integration cannot pick it up
   and have it silently appear to work. A later `initialize()` call (once
   the clock genuinely starts) retries the whole sequence from scratch -
   this outcome does not count as "already ready".
5. Wait &ge; 10 PCLK cycles (t<sub>mr</sub>) - computed from the
   configured PCLK rate, not hardcoded.
6. `/RESET` released (high).
7. Wait the PLL settle time (`Tsettle = 64 / FPCLK`).
8. `isReady()` becomes `true` and this call returns `Ready` - only now
   does `Si3050Controller::transferRaw()` forward to the SPI bus; before
   this, it returns `std::nullopt` without touching the bus at all.

The two waits in steps 5 and 7 are the only place a real, blocking
`delayMicroseconds()` call happens, and only during this one-time bring-up
- never in the main loop. They go through the injectable `IDelayProvider`
so native tests never actually sleep, and they are never reached unless
both fail-closed gates above have already passed.

## Ring detection

`RingDetector` (in `src/intercom/si3050/ring_detector.{h,cpp}`) polls
`ISi3050RingInput::readRaw()`, debounces it (50 ms default, configurable),
and reports `RingEvent::Asserted`/`RingEvent::Cleared` on a stable level
change. It does not touch audio, intercom line state, or publish any MQTT
event.

Important caveat, directly from the datasheet's "Ring Detection" section: during an
actual ring burst, `/RGDT` itself toggles at the ring signal's cadence
(tens of Hz), not a steady level. A simple debounced level reader like
this one will see that as a rapid string of asserted/cleared transitions,
not one clean "ring happened" event. That refinement - recognizing an
actual ring cadence vs. noise vs. a single edge - is exactly the kind of
real hardware/bench characterization this PR does not attempt. This class
only establishes the API shape (`update()` returning sanitized events)
that a future, bench-validated integration will build on.

## Tests

Two new native suites (mocks/fakes only - no Wi-Fi, broker, board, or real
Si3050 required):

- `test/test_si3050_controller/test_main.cpp` (16 tests): pin map
  compile-time contract exercised as runtime assertions too, the
  datasheet-derived timing formulas, the full bring-up call order via a
  shared `Si3050CallLog` across all four fakes, SCLK-before-reset-release
  and PCLK/FSYNC-before-reset-release as explicit ordering checks, the
  exact minimum-wait duration, `initialize()` idempotency, "CS born
  deselected", "no SPI transaction before ready", a mocked SPI read after
  readiness, and the fail-closed gates: a clock that starts but never
  reports `isRunning()` (`/RESET` stays asserted, no PCLK/PLL wait runs,
  `ClockNotRunning` returned), `pclkHz=0`, `fsyncHz=0` (both
  `InvalidConfig`, no division by zero), and a later `initialize()` retry
  succeeding once the fake clock is "fixed".
- `test/test_ring_detector/test_main.cpp` (5 tests): baseline
  establishment, asserted-after-debounce, cleared-after-debounce,
  short-noise-pulse rejection, and a configurable debounce interval.

## Future bring-up checklist (not executed in this PR)

Once Rev A hardware exists, in order:

1. **Power** - confirm 3.3 V rail(s) and isolation barrier supply are
   correct before connecting anything else.
2. **USB / flash** - confirm the board enumerates and accepts a firmware
   flash over native USB.
3. **BOOT / EN** - confirm the strap/reset behavior (GPIO9) still works
   for recovery after the Si3050 is wired in - it must never be claimed
   by the driver (enforced at compile time, see `si3050_pins.h`).
4. **SPI** - confirm `Esp32Si3050Bus` pin-level control (CS, SCLK
   idle-high) with a logic analyzer/scope; implement and verify a real
   `transfer()` once the SPI mode is confirmed.
5. **Reset / PCM** - confirm the documented electrical bring-up sequence
   against a scope (RESET pulse width, SCLK level at RESET, PCLK/FSYNC
   presence) and implement real PCM clock generation.
6. **RGDT** - confirm `/RGDT` idle level and behavior during a real
   incoming ring.
7. **Line** - characterize the actual analog intercom line and, only then,
   implement the DAA/line register configuration this PR deliberately
   left out.
8. **Audio** - codec/transport selection and implementation (still
   entirely open - see `src/audio/audio.h`).

Each step above depends on the previous one having been bench-confirmed;
none of them are validated by this PR.
