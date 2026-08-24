# Si3050 Clock Probe: ESP32-C3 -> ESP32 DevKitV1 (Phase 3B.1)

This document describes a bench-only experiment to validate two narrow
questions before any real Si3050 hardware exists:

1. Can an ESP32-C3 generate the Si3050's target PCM clocks (PCLK =
   2.048 MHz, FSYNC = 8 kHz, ratio 256) in hardware? **Open as of the
   latest real bench test** - see "Real bench observation: generator
   does not reach the target ratio" below; this is still being
   investigated, not confirmed.
2. Can a second board measure those clocks by hardware pulse counting and
   report believable frequencies/ratio back over serial? **Yes** - a real
   bench retest confirmed the PCNT meter (after its own bring-up fix,
   see "Real bench observation: PCNT bring-up order bug" below) reports a
   real, stable signal and ratio, even though that ratio is not yet the
   target one.

**This PR only creates a clock probe between two boards.** It does not
make the product's PCM clock functional, does not validate the Si3050
board, and does not change any decision about an external oscillator. See
`docs/si3050-bringup.md` for the actual Si3050 foundation - this probe is
a separate, isolated experiment that does not touch it.

## What this does NOT prove

The probe measures frequency and the PCLK:FSYNC ratio only. It does
**not** prove signal amplitude, noise, duty cycle, edge integrity, or fine
timing alignment between PCLK and FSYNC. It is not a substitute for an
oscilloscope or logic analyzer. No firmware here ever prints an absolute
"PASS".

## Isolation from the rest of the firmware

Two new PlatformIO environments, each built from an exclusive
`build_src_filter` (`-<*>` then only the specific files listed below), so
neither can ever compile `main.cpp`, the DEV MQTT smoke harness,
provisioning, or any other firmware path alongside it:

- `esp32-c3-si3050-clock-probe` - generator, compiles only
  `src/dev/si3050_clock_probe_generator_main.cpp`.
- `esp32dev-si3050-clock-meter` - meter, compiles only
  `src/dev/si3050_clock_probe_meter_main.cpp` and
  `src/dev/si3050_clock_probe_math.cpp`.

Neither environment depends on Wi-Fi. Neither touches
`Si3050Controller`, `Esp32PcmClock` (which remains an untouched stub), or
any GPIO/relay/RGDT/SPI/reset logic for a real Si3050. `esp32-c3` and
`esp32-c3-dev-mqtt` are unaffected - both new probe source files are
explicitly excluded from their `build_src_filter`s (and from `native`'s).

## Wiring

```text
ESP32-C3 GPIO0 (PCLK)  -> DevKitV1 GPIO34
ESP32-C3 GPIO1 (FSYNC) -> DevKitV1 GPIO35
ESP32-C3 GND            -> DevKitV1 GND
```

Power each board from its own USB port independently. **Do not tie the two
boards' 3V3 rails together** - only signal grounds are shared.

## Generator (ESP32-C3): how the clocks are requested

`src/dev/si3050_clock_probe_generator_main.cpp` configures the ESP32-C3's
I2S peripheral (via the ESP-IDF legacy `driver/i2s.h`, not Arduino's
higher-level `I2SClass`, which only exposes 2-channel Philips/MSB/PDM
modes) as a hardware TDM master:

- 16 TDM slots (`total_chan = 16`, `chan_mask` activating
  `I2S_TDM_ACTIVE_CH0..CH15`), 16 bits/sample
  (`I2S_BITS_PER_SAMPLE_16BIT`), sample rate = `Si3050Config::fsyncHz`
  (8000). Per the driver's own documented formula
  (`i2s_set_sample_rates()`'s doc comment in `driver/i2s.h`),
  `bit_clock = sample_rate * total_chan * bits_per_sample = 8000 * 16 *
  16 = 2,048,000 Hz` - **this is what is requested, not what a real bench
  test measured** - see "Real bench observation" below.
- `communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT`: documented in
  `hal/i2s_types.h` as "PCM Short standard, also known as DSP mode. The
  period of synchronization signal (WS) is 1 bck cycle" - this is the
  short, single-cycle frame pulse the Si3050 datasheet calls FSYNC, not
  I2S's own ~50%-duty Philips WS.
- BCLK routed to GPIO0 (`i2s_pin_config_t::bck_io_num`), WS routed to
  GPIO1 (`ws_io_num`) - the same pins `si3050_pins.h` assigns to
  PCLK/FSYNC for the real Si3050, since this probe is meant to validate
  generating a clock the real part would actually receive.
- Data content is irrelevant to this probe (only the clock timing
  matters) - `data_out_num = I2S_PIN_NO_CHANGE`, and
  `tx_desc_auto_clear = true` plus an initial `i2s_zero_dma_buffer()` call
  keep the peripheral emitting continuous silence rather than pausing on
  an empty TX buffer.
- After `i2s_set_pin()`, an explicit `i2s_set_clk(i2s_num, rate, bits_cfg,
  ch)` call re-applies the same total_chan/bits_per_sample via the
  driver's dedicated clock-configuration entry point - see "Real bench
  observation" below for why this was added and why it remains
  unconfirmed.

This configuration was designed against the framework version actually
installed in this repo (`framework-arduinoespressif32`
3.20017.241212+sha.dcc1105b, ESP-IDF 5.x legacy I2S driver), confirmed
before writing any code that the exposed API surface *could* express it:
`soc/esp32c3/include/soc/soc_caps.h` reports `SOC_I2S_SUPPORTS_TDM=1` for
ESP32-C3, and the vendored `driver/i2s.h`/`hal/i2s_types.h` expose the
`i2s_config_t` TDM fields (`chan_mask`, `total_chan`) and
`I2S_COMM_FORMAT_STAND_PCM_SHORT` needed above. No approximation
(LEDC/RMT/bit-banged `delay()`/`digitalWrite()` loop) is used. **A real
bench test of exactly this configuration did not reach the target
ratio** - see "Real bench observation: generator does not reach the
target ratio" below; do not read the bullets above as a claim that the
hardware delivers the target values, only that they are what is
requested from a documented API.

No sleeps, busy-waits, or approximations are used in the clock path
itself; `loop()` only idles (the I2S peripheral runs entirely in
hardware once configured).

## Meter (ESP32 DevKitV1): how the clocks are measured

`src/dev/si3050_clock_probe_meter_main.cpp` uses the ESP32's PCNT
(pulse counter) peripheral - never `digitalRead()` or a GPIO interrupt per
edge, which cannot keep up with 2.048 MHz.

- One PCNT unit per signal (`PCNT_UNIT_0` for PCLK/GPIO34, `PCNT_UNIT_1`
  for FSYNC/GPIO35), counting rising edges only
  (`PCNT_COUNT_INC`/`PCNT_COUNT_DIS`), no control-signal gating.
- **No glitch filter.** `pcnt_filter_enable()`/`pcnt_set_filter_value()`
  are never called - the filter is measured in APB clock cycles and could
  silently eat legitimate ~244 ns half-cycles at 2.048 MHz.
- **Overflow-safe counting.** The PCNT hardware counter register is only
  16-bit signed (`pcnt_config_t::counter_h_lim` is an `int16_t`), so a
  useful measurement window at 2.048 MHz vastly exceeds it. Each unit is
  configured with a `PCNT_EVT_H_LIM` watchpoint at
  `kClockProbePcntHighLimit` (30000, in
  `src/dev/si3050_clock_probe_math.h`, safely inside the `int16_t`
  range) and a shared ISR that increments a software overflow counter and
  clears the hardware counter every time the watchpoint fires. The true
  edge count for a window is `overflowCount * hLimit + rawCount`
  (`combinePulseCount()`), read and reset atomically with the ISR excluded
  (`portENTER_CRITICAL`/`portEXIT_CRITICAL` around the sample) so a
  concurrent overflow can never produce a torn read.
- **Bring-up order and fail-closed error handling.** Both PCNT units are
  fully configured/paused/cleared *before* the ISR service is installed,
  which is itself installed before either handler is added, which is
  done before either unit's counting is actually resumed - see "Real
  bench observation" below for why this order matters and is not
  optional. Every relevant ESP-IDF call's `esp_err_t` is checked
  (`PcntBringupTracker` in `src/dev/si3050_clock_probe_meter_bringup.{h,
  cpp}`); the first failure stops bring-up immediately, prints one
  sanitized `pcnt bringup failed step=... esp_err=...` line, and leaves
  `meter_started=false` - `loop()` never reports a measurement or stats
  line in that state, and `setup()` never prints `pcnt configured`
  unless every step genuinely succeeded.
- **Real, measured window duration.** The window boundary comes from
  `esp_timer_get_time()` (a monotonic microsecond counter), not an
  assumed exact 1000 ms - `loop()`'s own polling/`Serial.printf()` jitter
  is absorbed into the reported `window_us`, never silently ignored.
  Serial is used only to report results, never for timing or counting.
- Reports approximately once per second: current window's edge counts,
  computed PCLK/FSYNC frequencies, and ratio, plus running
  minimum/maximum statistics across the whole bench session
  (`ClockProbeMinMaxTracker`).

## Build / flash / monitor

```bash
# Generator (flash to the ESP32-C3)
pio run -e esp32-c3-si3050-clock-probe -t upload
pio device monitor -b 115200

# Meter (flash to the ESP32 DevKitV1, on its own USB port)
pio run -e esp32dev-si3050-clock-meter -t upload
pio device monitor -b 115200
```

No flashing was performed while writing any version of this PR - only
`pio run` (compile-only) was executed in-session. Real bench boots of
both boards (outside this session, with all three wires connected) are
what found the PCNT bring-up bug, confirmed its fix, and found that the
generator does not reach the target ratio - see the two "Real bench
observation" sections below. See the PR description for the exact
commands run in-session and their results.

## Expected output

Generator, once at startup - reports what was *requested*, never a
frequency claim (see "Real bench observation" below):

```text
[SI3050 CLOCK PROBE] requested_sample_rate_hz=8000 requested_total_chan=16 requested_bits_per_sample=16 requested_ratio=256 requested_pclk_hz=2048000 started=true
[SI3050 CLOCK PROBE] note: the line above reports what was requested from the I2S driver, not a measurement - only esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies
```

Meter, roughly once per second - this is an **actual real bench
measurement** of the current generator configuration (see "Real bench
observation" below for the full analysis; `pclk_hz`/`fsync_hz`/`ratio`
are not yet at the target values):

```text
[SI3050 CLOCK METER] pcnt configured pclk_pin=34 fsync_pin=35 h_lim=30000
[SI3050 CLOCK METER] window_us=1000091 pclk_edges=1024145 pclk_hz=1024082.6 fsync_edges=16003 fsync_hz=16001.5 ratio=63.996
[SI3050 CLOCK METER] stats pclk_hz_min=1023991.0 pclk_hz_max=1024211.0 fsync_hz_min=15999.3 fsync_hz_max=16006.8 ratio_min=63.993 ratio_max=64.001
```

If the generator side is later corrected to actually reach the target
geometry, the values to look for would be `pclk_hz` near 2,048,000,
`fsync_hz` near 8,000, and `ratio` near 256 - real crystal/clock
tolerance and USB-CDC/print jitter mean exact integers should never be
expected either way, so the min/max stats over a multi-minute run are
what actually characterize the clock's stability, not any single line.

Meter, if PCNT bring-up fails (see "Real bench observation" below - this
is the actual failure mode a first real boot hit):

```text
[SI3050 CLOCK METER] pcnt bringup failed step=pcnt_isr_service_install esp_err=-1
[SI3050 CLOCK METER] meter_started=false - no measurement will be reported
```

No `pcnt configured`, `window_us=...`, or `stats ...` line is ever printed
in this state - the exact single line above is the one to look for when
confirming the fix on a real board.

After a bench run of a few minutes, copy the full serial log (both
boards, plus the min/max lines) into the bench validation notes -
including the first and last `stats` line so the drift/spread over the
whole run is visible, not just one snapshot.

## Real bench observation: PCNT bring-up order bug

The first real boot of the meter on a DevKitV1 logged this and then went
on to (wrongly) print `pcnt configured` regardless:

```text
E (11) pcnt: _pcnt_isr_service_install(316): PCNT driver error
E (13) pcnt: _pcnt_isr_handler_add(264): ISR service is not installed, call pcnt_install_isr_service() first
E (16) pcnt: _pcnt_isr_handler_add(264): ISR service is not installed, call pcnt_install_isr_service() first
[SI3050 CLOCK METER] pcnt configured ...
```

The original bring-up order installed the ISR service (`pcnt_isr_service_
install()`) *before* either PCNT unit had been configured
(`pcnt_unit_config()`), which the installed driver rejects outright
("PCNT driver error"); every following `pcnt_isr_handler_add()` call then
failed too, since the service was never actually installed - so neither
unit's overflow handler existed at all. None of the original code checked
any of these return values, so the firmware printed `pcnt configured` and
would have gone on to report fabricated measurements built from a PCNT
counter that could silently wrap past its `int16_t` limit with no
overflow ISR to catch it.

Fixed in `src/dev/si3050_clock_probe_meter_main.cpp` by reordering to the
sequence documented above (configure/pause/clear both units, then install
the ISR service, then add handlers, then enable events, then resume), and
by checking every relevant call's `esp_err_t`
(`pcnt_unit_config`/`pcnt_counter_pause`/`pcnt_counter_clear`/
`pcnt_event_enable`/`pcnt_isr_service_install`/`pcnt_isr_handler_add`/
`pcnt_counter_resume`) via the new, natively-tested
`PcntBringupTracker`. `pcnt_isr_service_install()`'s own documented
`ESP_ERR_INVALID_STATE` ("ISR service already installed") is the one
specific code treated as "ready to proceed" - `isPcntIsrServiceReady()`
- every other non-`ESP_OK` code from any step is a hard failure: bring-up
stops immediately, one sanitized `pcnt bringup failed step=... esp_err=...`
line is printed, and `meter_started=false` - `setup()` never prints `pcnt
configured` and `loop()` never prints a `window_us=.../stats ...` line
in that state.

**Confirmed on real hardware**: a physical retest (all three wires
connected: C3 GPIO0 -> DevKit GPIO34, C3 GPIO1 -> DevKit GPIO35, GND ->
GND) validated this fix - the meter printed `pcnt configured` and
reported a real, stable signal (see "Real bench observation: generator
does not reach the target ratio" below for the actual numbers). The PCNT
bring-up fix itself is validated; the ratio those numbers show is not
yet the target one, which is a separate, generator-side issue.

## Real bench observation: generator does not reach the target ratio

The same physical retest that confirmed the PCNT fix also measured the
generator's actual output for the first time. With the three wires
connected as above, the meter stabilized on:

```text
pclk_hz  ~= 1,024,127
fsync_hz ~= 16,003
ratio    ~= 63.996
```

- The signal is real and stable, and the meter (now bring-up-fixed) is
  trustworthy - this is a genuine measurement, not noise or a PCNT
  artifact.
- It is **not** the requested geometry
  (`total_chan=16`, `bits_per_sample=16`, `sample_rate=8000` ->
  requested `pclk_hz=2,048,000`, `fsync_hz=8,000`, `ratio=256`).
  `fsync_hz` measured at ~2x the requested sample rate; `pclk_hz`
  measured at ~1/2 the requested value; `ratio` measured at ~1/4 the
  requested value (64, not 256).
- This falsifies the earlier assumption (removed from this document -
  see "Generator" above) that `I2S_CHANNEL_FMT_MULTIPLE` +
  `total_chan=16` + `chan_mask` (all 16 TDM slots) + 16-bit samples +
  `I2S_COMM_FORMAT_STAND_PCM_SHORT` would automatically deliver a 16 x 16
  = 256 ratio on this driver/chip combination, even though every
  individual field is individually documented and the combination
  compiles and installs without error.

### Investigation (source not available - precompiled library only)

`framework-arduinoespressif32`'s `driver` component ships as a
precompiled `tools/sdk/esp32c3/lib/libdriver.a` in this PlatformIO
package - there is no `.c` source for the legacy I2S driver in this
repository's toolchain to read the actual clock-divider computation.
Everything below is from the installed **headers only**, which is why
this section documents a blocker and a best-effort attempt rather than a
confirmed fix:

- `hal/i2s_hal.h`'s `i2s_hal_config_t` has a `total_chan` field (Total
  number of I2S channels) **and a separate `active_chan` field** (I2S
  active channel number) - two different concepts. The public legacy
  `i2s_config_t` used by `i2s_driver_install()` only exposes `total_chan`
  and `chan_mask`; there is no direct way to set `active_chan` through
  it.
- `i2s_set_clk(i2s_port_t i2s_num, uint32_t rate, uint32_t bits_cfg,
  i2s_channel_t ch)` (`driver/i2s.h`) is documented with a `ch` parameter
  described as accepting "`I2S_CHANNEL_MONO`, `I2S_CHANNEL_STEREO` or
  specific channel in TDM mode" - i.e. this function, not
  `i2s_driver_install()` alone, appears to be the documented way to
  (re)apply the TDM channel bitmask specifically for clock configuration.
  The generator's original code never called it.
- At the register level (`hal/esp32c3/include/hal/i2s_ll.h`),
  `i2s_ll_tx_set_active_chan_mask()` does write `chan_mask` into the
  hardware TDM control register (`I2S_LL_TDM_CH_MASK = 0xffff`, no
  truncation there), so `total_chan`/`chan_mask` are not simply dropped
  at the LL layer - but this does not by itself prove they reach the
  `mclk_div`/`bclk_div` clock-divider computation
  (`i2s_ll_tx_set_clk()`/`i2s_hal_tx_clock_config()`), which is exactly
  where the source is unavailable.

**Attempted, driver-justified adjustment** (not a guessed/invented
configuration - it requests the same `total_chan=16`/`bits_per_sample=16`
already set, via the driver's own dedicated clock API): added an explicit
`i2s_set_clk(kI2sPort, kConfig.fsyncHz, bitsCfg, chan_mask_as_channel_t)`
call after `i2s_set_pin()` in
`src/dev/si3050_clock_probe_generator_main.cpp`, checked like every other
step. **This is unconfirmed** - it has not been re-tested on real
hardware in this session, and given the exact ~1/4 ratio and ~2x FSYNC
already observed do not match any single, simple, headers-only-derivable
formula with full confidence, it may not resolve the discrepancy.

### What the generator now logs

Startup diagnostics were expanded per this investigation - see "Expected
output" below for the exact lines. They report what was *requested*
(`requested_sample_rate_hz`, `requested_total_chan`,
`requested_bits_per_sample`, `requested_ratio`, `requested_pclk_hz`) and
whether the driver *accepted* that request (`started=true/false`, plus
each call's `esp_err_t` on failure) - never a frequency claim. The
`requested_ratio`/`requested_pclk_hz` values are computed via
`configuredTdmRatio()`/`configuredBclkHz()`
(`src/dev/si3050_clock_probe_generator_config.h`, natively tested) so the
log can never silently drift from the actual `total_chan`/`bits_per_sample`
values used to configure the driver.

### Minimal alternatives for a future decision (none implemented here)

Per the explicit instruction not to implement a workaround/approximation
without documented justification, none of the following were
implemented - they are presented for a future decision once the
`i2s_set_clk()` attempt above has been re-tested on real hardware:

1. **Re-test first.** The `i2s_set_clk()` addition is a real,
   driver-documented candidate that was not present in the configuration
   that produced the ratio-64 measurement - it needs its own bench run
   before concluding anything further is needed.
2. **Move off the legacy compatibility driver.** This framework version
   only vendors `driver/i2s.h` (the legacy compatibility shim); it does
   not vendor the newer, natively-documented `driver/i2s_std.h`/
   `driver/i2s_tdm.h` components that ESP-IDF 5.x ships for direct,
   better-specified TDM control. Building against ESP-IDF directly (or a
   newer Arduino-ESP32 core release, if one vendors these headers) may
   expose the missing control surface - this is a toolchain/framework
   version decision, not a firmware code change.
3. **Reconsider the target geometry.** If neither of the above closes
   the gap, decide whether the Si3050 bring-up work actually requires
   this exact 2.048 MHz/8 kHz/256:1 relationship from *this specific*
   ESP32-C3 peripheral, or whether an external clock generator/oscillator
   for the Si3050's PCM interface is the more realistic path for Rev A -
   a hardware decision, out of this firmware's scope.

## Tests

`test/test_si3050_clock_probe_math/test_main.cpp` (7 tests, native, no
hardware): the target ratio (256) from raw edge counts; a full
`ClockProbeWindowResult` at target frequencies; frequency conversion
using a real (non-1-second) window duration; zero/invalid counts and
windows never dividing by zero; overflow-cycle combination plus an
out-of-range raw count treated as invalid rather than producing a huge or
wrong total; and the min/max tracker (including that non-finite samples
are ignored). `TEST_ASSERT_EQUAL_DOUBLE` required adding
`-DUNITY_INCLUDE_DOUBLE` to `[env:native]` in `platformio.ini` (Unity
disables double-precision assertions by default) - this is the first
suite in this repo to use floating-point assertions.

`test/test_si3050_clock_probe_meter_bringup/test_main.cpp` (6 tests,
native, no hardware): `isPcntIsrServiceReady()` on success, on the
documented "already installed" code, and on a genuine unrelated failure
code; `PcntBringupTracker` starting without a failure; never failing when
every recorded step succeeds; and recording only the first failure (a
later failure or success never overwrites the original root cause). The
real PCNT calls themselves are not exercised here - only the pure
decision/tracking logic around them; see "Real bench observation: PCNT
bring-up order bug" above.

`test/test_si3050_clock_probe_generator_config/test_main.cpp` (4 tests,
native, no hardware): `configuredTdmRatio()`/`configuredBclkHz()` at the
requested geometry (16 channels x 16 bits -> ratio 256, BCLK 2,048,000)
and as pure multiplication for other inputs (including zero). These
functions compute what the generator *requests* and logs - they say
nothing about what the hardware actually produces; see "Real bench
observation: generator does not reach the target ratio" above.

Grep-verified (not itself a compiled test, since it is a structural/
negative property): neither `si3050_clock_probe_math.{h,cpp}`,
`si3050_clock_probe_generator_main.cpp`,
`si3050_clock_probe_generator_config.h`,
`si3050_clock_probe_meter_main.cpp`, nor
`si3050_clock_probe_meter_bringup.{h,cpp}` references Wi-Fi, MQTT,
`IHardwareIO`, `setDoorOutput`, or any door-actuation symbol. See the PR
description for the exact command and its (empty) result.

## Known limitations / not yet done

- **The meter's PCNT bring-up fix is confirmed on real hardware** (a
  physical retest with all three wires connected printed `pcnt
  configured` and reported a real, stable signal) - see "Real bench
  observation: PCNT bring-up order bug" above.
- **The generator does not yet reach the target PCLK/FSYNC/ratio on real
  hardware**, and the `i2s_set_clk()` addition attempted in response is
  itself unconfirmed - only native tests and `pio run` (compile-only)
  verify it compiles in this session, not a fresh bench boot. See "Real
  bench observation: generator does not reach the target ratio" above,
  including the minimal alternatives presented for a future decision if
  a retest still does not close the gap.
- The atomic ISR-excluded sample in the meter still allows a handful of
  PCLK edges to go uncounted during the brief critical section on every
  window boundary (interrupts, including the overflow ISR, are disabled
  for that duration) - negligible at a ~1 Hz reporting cadence, but worth
  knowing if the reported `pclk_hz` reads very slightly (a few Hz) low
  over many windows.
- This probe says nothing about whether the *real* Si3050 part, once
  physically present, would accept these signals correctly (electrical
  levels, the datasheet's other AC timing requirements, or the DAA
  configuration this PR still does not implement - see
  `docs/si3050-bringup.md`).
