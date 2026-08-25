# Si3050/Si3011-19 Firmware Foundation (Phase 3A)

This document describes the Phase 3A firmware foundation for the Si3050
DAA (+ Si3011/18/19 line-side device) that will interface the ESP32-C3 to
the analog intercom line on hardware Rev A. It started as a **testable
foundation, not a working driver**: validated with mocks and compilation
only, no Rev A board in existence, nothing run against real hardware. A
follow-up pass (Phase 3B.2, this section) changed that for **one specific
piece - PCM clock generation** - see "PCM clock: validation status" below
for exactly what is and is not proven now. Everything else described
below (register access, real SPI, ring pattern/off-hook/line/audio) is
still unvalidated and still deferred.

## Scope

Implemented:

- The hardware-independent Si3050 electrical **bring-up sequence**
  (`Si3050Controller`), gating SPI access on it having completed.
- Narrow HAL interfaces for the four things that sequence needs (SPI bus +
  chip select, PCM clock, `/RESET`, and a short injectable delay), each
  with a real ESP32-C3 implementation and a deterministic fake for tests.
- **Real PCM clock generation** (`Esp32PcmClock`, Phase 3B.2): a
  1.024 MHz PCLK / 8 kHz FSYNC, 16 x 8 TDM, PCM-short I2S configuration -
  first physically validated on real ESP32-C3 hardware in an isolated
  bench probe, then integrated into the normal firmware's boot sequence
  (`src/main.cpp`) and separately reflashed and measured there. See "PCM
  clock: validation status" below for the precise scope of what this does
  and does not prove.
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
- **PCM audio data** (`DRX`/`DTX`) and any audio content. `Esp32PcmClock`
  generates the PCLK/FSYNC clock signal only - the I2S data pins are left
  unrouted (`I2S_PIN_NO_CHANGE`), matching the validated probe.
- Real SPI transactions (`Esp32Si3050Bus::transfer()`): the SPI clock
  polarity/phase the Si3050 expects has not been confirmed against real
  hardware here, so it remains an explicit `TODO` rather than a guess.
- Real ring **pattern** validation, off-hook detection, line
  characterization, or audio. `RingDetector` reports a debounced
  electrical level change only - see "Ring detection" below for exactly
  why that is not the same as a validated ring. `RingDetector` is not
  wired into `src/main.cpp` by this pass either.
- Anything touching `IHardwareIO`, `setDoorOutput()`, or any door
  actuation path. The Si3050 module has no dependency on any of that by
  construction (verified by grep, not just inspection).
- Any change to Wi-Fi, BLE, MQTT, AWS, provisioning, remote commands, or
  reconnection behavior.

## PCM clock: validation status

Three distinct claims, kept explicitly separate so none is overstated:

1. **Validated on the physical probe.** The exact `1.024 MHz` PCLK /
   `8 kHz` FSYNC / `16 x 8` TDM / PCM-short geometry was flashed to a
   real ESP32-C3 and measured externally by an independent ESP32
   DevKitV1 running hardware pulse counting (PCNT) - see
   docs/si3050-clock-probe.md's "Real bench observation: 16 x 8 slot
   geometry reaches the PCM/SPI target". That happened in the isolated
   `esp32-c3-si3050-clock-probe` bench environment, a throwaway I2S
   configuration written directly in `src/dev/`, not through
   `Esp32PcmClock` or `Si3050Controller`.
2. **The real integrated implementation is also physically validated.** `Esp32PcmClock`
   (`src/intercom/si3050/si3050_pcm_clock.{h,cpp}`) now implements the
   same geometry for real (not a stub), and `src/main.cpp` constructs
   `Si3050Controller` with it and calls `initialize()` during `setup()`
   (see "PCM clock integration" below). The normal `esp32-c3` environment
   containing this real implementation has now been reflashed and measured
   physically, producing approximately `1.024 MHz` PCLK / `8 kHz` FSYNC /
   `128` clocks per frame. It also remains compiled and linked by the
   existing CI coverage, while its bring-up/rollback/idempotency decision
   logic is covered by native tests (`test/test_si3050_pcm_clock/`). This is
   a separate validation of the integrated binary, not an inference from
   the isolated probe result.
3. **Still not validated against a real Si3050.** No Si3050 or Si3011/18/19
   part has been connected or initialized at any point, in either the
   probe or this integration. `Esp32Si3050Bus::transfer()` (real SPI)
   remains an unimplemented `TODO`, so even once `Si3050Controller::
   initialize()` reports `Ready`, no register has been read from or
   written to a real part. DAA registers, `DRX`/`DTX`, audio, ring,
   off-hook, and relay behavior therefore remain outside scope and
   unvalidated.

## PCM clock integration

`src/main.cpp` constructs `Esp32Si3050Bus`, `Esp32PcmClock`,
`Esp32Si3050Reset`, `Esp32Si3050Delay`, and `Si3050Controller` inside
`initializeSi3050()` (called from `setup()`, after `initializeHardware()`
and before `initializeNetwork()`) - not as global objects like the other
hardware singletons in that file, because `Esp32Si3050Bus`'s and
`Esp32Si3050Reset`'s constructors call real `pinMode()`/`digitalWrite()`
under `#ifdef ARDUINO`, which must not run before the Arduino runtime
itself has initialized (before `setup()` starts). They are held in
`std::optional<T>` and `.emplace()`d inside `initializeSi3050()` instead,
mirroring the existing `std::optional`+`.emplace()` pattern already used
elsewhere in that file for objects needing deferred construction (e.g.
`mqttTopics`, `commandHandler`, `provisioningManager`).

`initializeSi3050()` calls `si3050Controller->initialize()` once, at
every boot, unconditionally - there is no feature flag gating it. This
is safe to run with no Si3050 physically attached: the sequence only
deselects CS, asserts then (once the clock is confirmed running)
releases `/RESET`, holds SCLK high, and starts the PCLK/FSYNC clock -
all plain GPIO/I2S operations with no real chip required to be present,
and no register access is attempted. The `Si3050InitResult` outcome
(`Ready`/`ClockNotRunning`/`InvalidConfig`) is logged via `Logger` only -
it does not affect the state machine, MQTT, or any protocol event.
There is currently no automatic retry if `initialize()` fails at boot;
that remains a documented gap, not a silently-missing feature (see
"Known gaps" below).

**Pinning used:** `si3050_pins.h`'s existing Rev A pin map - GPIO0
(PCLK) and GPIO1 (FSYNC), the same two pins the physical probe used and
validated. This pin map was already established (with
compile-time-checked reserved-pin collision checks) before this pass;
this integration did not invent or guess any pin assignment, it reused
the existing source of truth. This is a different, Si3050-specific pin
map from the general `Esp32GpioHardware` abstraction (used for the
door/relay/button path), which remains an undefined stub - that
separate, still-open pinning question is unaffected by this pass.

**Known gaps before this can be considered production-ready:**

- No automatic retry of `Si3050Controller::initialize()` if it fails at
  boot (e.g. `ClockNotRunning` from a real hardware fault) - a later
  reboot is currently the only recovery path.
- No real Si3050 has been connected, so the SCLK-level-at-RESET mode
  selection, PLL settle timing, and DAA register sequence that follow
  clock bring-up remain unconfirmed against actual part behavior.

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
   returns `ClockNotRunning`. `Esp32PcmClock` now implements a real
   `start()` (see "PCM clock integration" above): `isRunning()` reports
   `true` only once every I2S bring-up step it performs has actually
   succeeded (checked via `Si3050PcmClockBringup`), and reports `false`
   on any failure or on the native (host) build, where no real I2S
   peripheral exists - so the controller still structurally cannot
   report `Ready` unless the clock genuinely started. A later
   `initialize()` call (once the clock genuinely starts) retries the
   whole sequence from scratch - this outcome does not count as
   "already ready".
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

Native suites (mocks/fakes only - no Wi-Fi, broker, board, or real
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
  succeeding once the fake clock is "fixed". Uses `FakePcmClock`
  throughout, so these tests are unaffected by `Esp32PcmClock` becoming a
  real implementation.
- `test/test_si3050_pcm_clock/test_main.cpp` (21 tests): the validated
  TDM geometry's pure math (`16 x 8 = 128` clocks/frame,
  `8000 x 128 = 1,024,000` Hz); the fail-closed configuration gate
  (`si3050PcmConfigurationSupported()`) accepting the validated target and
  rejecting the old GCI-style target, a zero FSYNC, and a mismatched
  PCLK/FSYNC pair; `Si3050PcmClockBringup`'s pure bring-up/rollback/
  idempotency decision logic - success when every step succeeds, failure
  at each of the three bring-up steps with the correct rollback-owed
  state, first-failure-wins, rollback resetting state for a clean retry,
  repeated stop() being safe, and `markRunning()` never overriding a
  failed attempt; and `Esp32PcmClock` on the native (host) build never
  claiming `isRunning()==true` without real hardware, plus safe repeated
  `stop()`/`start()`-without-hardware calls. The real ESP-IDF I2S calls
  themselves are not exercised here - only the pure decision/tracking
  logic around them (same limitation as `test_si3050_clock_probe_meter_bringup`
  has for the clock probe meter) - see "PCM clock: validation status"
  above for what real-hardware validation exists instead.
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
   presence). PCM clock generation itself is now implemented and
   integrated (`Esp32PcmClock`, see "PCM clock integration" above) and
   both its geometry and the integrated normal-`esp32-c3` clock output are
   physically validated. What remains here is confirming the electrical
   sequence's other timings (RESET pulse width, SCLK level at RESET) once
   a real Si3050 is present to observe.
6. **RGDT** - confirm `/RGDT` idle level and behavior during a real
   incoming ring.
7. **Line** - characterize the actual analog intercom line and, only then,
   implement the DAA/line register configuration this PR deliberately
   left out.
8. **Audio** - codec/transport selection and implementation (still
   entirely open - see `src/audio/audio.h`).

Each step above depends on the previous one having been bench-confirmed.
None of them require a real Si3050 to already have happened. The PCM
clock's *geometry* and the real integrated `Esp32PcmClock` output are the
exceptions already physically validated (see "PCM clock: validation
status" above); none of the Si3050-dependent checks are validated.
