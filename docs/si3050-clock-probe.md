# Si3050 Clock Probe: ESP32-C3 -> ESP32 DevKitV1 (Phase 3B.1)

This document describes a bench-only experiment to validate two narrow
questions before any real Si3050 hardware exists:

1. Can an ESP32-C3 generate the Si3050's target PCM clocks (PCLK =
   2.048 MHz, FSYNC = 8 kHz, ratio 256) in hardware, using a documented,
   verified peripheral configuration?
2. Can a second board measure those clocks by hardware pulse counting and
   report believable frequencies/ratio back over serial?

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

## Generator (ESP32-C3): how the clocks are actually produced

`src/dev/si3050_clock_probe_generator_main.cpp` configures the ESP32-C3's
I2S peripheral (via the ESP-IDF legacy `driver/i2s.h`, not Arduino's
higher-level `I2SClass`, which only exposes 2-channel Philips/MSB/PDM
modes) as a hardware TDM master:

- 16 TDM slots (`total_chan = 16`, `chan_mask` activating
  `I2S_TDM_ACTIVE_CH0..CH15`), 16 bits/sample
  (`I2S_BITS_PER_SAMPLE_16BIT`), sample rate = `Si3050Config::fsyncHz`
  (8000). `bit_clock = sample_rate * total_chan * bits_per_sample =
  8000 * 16 * 16 = 2,048,000 Hz` - the driver's own documented formula
  (`i2s_set_sample_rates()`'s doc comment in `driver/i2s.h`).
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

This was confirmed against the framework version actually installed in
this repo (`framework-arduinoespressif32` 3.20017.241212+sha.dcc1105b,
ESP-IDF 5.x legacy I2S driver) before writing any code:
`soc/esp32c3/include/soc/soc_caps.h` reports `SOC_I2S_SUPPORTS_TDM=1` for
ESP32-C3, and the vendored `driver/i2s.h`/`hal/i2s_types.h` expose the
`i2s_config_t` TDM fields (`chan_mask`, `total_chan`) and
`I2S_COMM_FORMAT_STAND_PCM_SHORT` needed above. No approximation
(LEDC/RMT/bit-banged `delay()`/`digitalWrite()` loop) was used or needed -
the hardware genuinely supports the exact target ratio via a documented
configuration.

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

No flashing was performed while writing either version of this PR - only
`pio run` (compile-only) was executed in-session. A real bench boot of
the meter (outside this session) is what found the PCNT bring-up bug
described in "Real bench observation" below. See the PR description for
the exact commands run and their results.

## Expected output

Generator, once at startup:

```text
[SI3050 CLOCK PROBE] pclk_target_hz=2048000 fsync_target_hz=8000 ratio_target=256 started=true
```

Meter, roughly once per second:

```text
[SI3050 CLOCK METER] pcnt configured pclk_pin=34 fsync_pin=35 h_lim=30000
[SI3050 CLOCK METER] window_us=1000123 pclk_edges=2048252 pclk_hz=2048005.2 fsync_edges=8001 fsync_hz=8000.0 ratio=256.058
[SI3050 CLOCK METER] stats pclk_hz_min=2047998.1 pclk_hz_max=2048011.4 fsync_hz_min=7999.9 fsync_hz_max=8000.1 ratio_min=255.998 ratio_max=256.012
```

Expected values, once wired to a real board pair: `pclk_hz` near
2,048,000; `fsync_hz` near 8,000; `ratio` near 256. Real crystal/clock
tolerance and USB-CDC/print jitter mean exact integers should not be
expected - the min/max stats over a multi-minute run are what actually
characterizes the clock's stability, not any single line.

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
in that state. **This fix has not yet been re-verified on real hardware
in this session** - the corrected order matches the driver's documented
contract and the observed failure mode, and compiles for both new
environments, but a fresh bench boot confirming `pcnt configured` (or a
clean, honest `meter_started=false` if something is still wrong) is still
required before this counts as validated.

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
decision/tracking logic around them; see "Real bench observation" above
for what remains unverified on real hardware.

Grep-verified (not itself a compiled test, since it is a structural/
negative property): neither `si3050_clock_probe_math.{h,cpp}`,
`si3050_clock_probe_generator_main.cpp`, nor
`si3050_clock_probe_meter_main.cpp` references Wi-Fi, MQTT, `IHardwareIO`,
`setDoorOutput`, or any door-actuation symbol. See the PR description for
the exact command and its (empty) result.

## Known limitations / not yet done

- **The meter's PCNT bring-up fix has not been re-verified on real
  hardware yet.** The first real DevKitV1 boot (outside this session) is
  what found the bring-up-order bug described above; the fix itself was
  verified by native tests, by reading the installed driver's actual
  documented error contract, and by `pio run` (compile-only) for both
  environments in this session - not by a fresh bench boot. The generator
  has not been flashed or run on real hardware at all yet.
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
