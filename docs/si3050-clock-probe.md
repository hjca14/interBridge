# Si3050 Clock Probe: ESP32-C3 -> ESP32 DevKitV1 (Phase 3B.1)

This document describes a bench-only experiment to validate two narrow
questions before any real Si3050 hardware exists:

1. Can an ESP32-C3 generate the Si3050's target PCM clocks (PCLK =
   2.048 MHz, FSYNC = 8 kHz, ratio 256) in hardware? **Not with the
   legacy driver, as tested; likely not at all on this chip, per its own
   downloaded driver source.** Two real bench tests of the legacy
   `driver/i2s.h` (the original TDM configuration, then an additional
   `i2s_set_clk()` adjustment) measured an identical ~1.024 MHz/~16 kHz/
   ~64:1 output instead of the target - see "Real bench observation:
   generator does not reach the target ratio" below. A follow-up
   investigation downloaded a real ESP-IDF 6.0.1 toolchain and confirmed,
   from the modern native TDM driver's own enforcing source code, that
   ESP32-C3's I2S TDM hardware has a genuine 128-bit-per-frame ceiling -
   half of the 256 bits/frame the target ratio needs - see "IDF native
   TDM driver investigation" below. This is a real hardware limit, not
   fixable by switching drivers alone.
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

Three PlatformIO environments, none of which can ever compile `main.cpp`,
the DEV MQTT smoke harness, provisioning, or any other firmware path
alongside it:

- `esp32-c3-si3050-clock-probe` - legacy-driver generator, compiles only
  `src/dev/si3050_clock_probe_generator_main.cpp`.
- `esp32dev-si3050-clock-meter` - meter, compiles only
  `src/dev/si3050_clock_probe_meter_main.cpp`,
  `src/dev/si3050_clock_probe_math.cpp`, and
  `src/dev/si3050_clock_probe_meter_bringup.cpp`.
- `esp32-c3-si3050-clock-probe-idf5` - native-IDF-driver generator,
  compiles only `src/dev/si3050_clock_probe_generator_idf5_main.cpp`.

The first two (both `framework = arduino`) use an exclusive
`build_src_filter` (`-<*>` then only the specific files above) for
isolation. The third (`framework = espidf`) cannot use
`build_src_filter` at all - PlatformIO ignores it for ESP-IDF projects -
so it is isolated instead via `src/CMakeLists.txt`, a new file that lists
only that one source file for the ESP-IDF "main" component; see "IDF
native TDM driver investigation" below for why that file was needed and
confirmation (by rebuilding every other environment and comparing binary
sizes) that it does not affect any Arduino-framework environment.

None of the three environments depends on Wi-Fi. None touches
`Si3050Controller`, `Esp32PcmClock` (which remains an untouched stub), or
any GPIO/relay/RGDT/SPI/reset logic for a real Si3050. `esp32-c3` and
`esp32-c3-dev-mqtt` are unaffected - all three probe source files are
explicitly excluded from their `build_src_filter`s (and from `native`'s),
and neither uses `framework = espidf` or is otherwise touched by this
PR's toolchain investigation.

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

Only `i2s_driver_install()` + `i2s_set_pin()` are used - an additional
`i2s_set_clk()` call was tried and removed after a real bench retest
showed it does not change the output; see "Real bench observation"
below.

This configuration was designed against the framework version actually
installed in this repo (`framework-arduinoespressif32`
3.20017.241212+sha.dcc1105b, built on ESP-IDF 4.4.7 - see "Toolchain
investigation" below for exactly how that was confirmed - using its
legacy `driver/i2s.h`, the only I2S driver this framework version has),
confirmed before writing any code that the exposed API surface *could*
express it:
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
# Legacy-driver generator (flash to the ESP32-C3)
pio run -e esp32-c3-si3050-clock-probe -t upload
pio device monitor -b 115200

# Native-IDF-TDM-driver generator (flash to the SAME ESP32-C3 instead -
# only one generator firmware runs at a time; re-flash to switch between
# them)
pio run -e esp32-c3-si3050-clock-probe-idf5 -t upload
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

Legacy-driver generator, once at startup - reports what was *requested*,
never a frequency claim (see "Real bench observation" below):

```text
[SI3050 CLOCK PROBE] requested_sample_rate_hz=8000 requested_total_chan=16 requested_bits_per_sample=16 requested_ratio=256 requested_pclk_hz=2048000 started=true
[SI3050 CLOCK PROBE] note: the line above reports what was requested from the I2S driver, not a measurement - only esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies
```

Native-IDF-TDM-driver generator, once at startup - same reporting
pattern, same disclaimer. Given `total_slots x bits_per_sample = 256`
exceeds the ESP32-C3's documented 128-bit TDM frame limit (see "IDF
native TDM driver investigation" below), `started=false` is the
*predicted* outcome, unconfirmed until a real bench boot:

```text
[SI3050 CLOCK PROBE IDF5] requested_sample_rate_hz=8000 requested_total_slots=16 requested_bits_per_sample=16 requested_ratio=256 requested_pclk_hz=2048000 started=false
[SI3050 CLOCK PROBE IDF5] note: the line above reports what was requested, not a measurement - only esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies
[SI3050 CLOCK PROBE IDF5] i2s_new_channel=0 i2s_channel_init_tdm_mode=258 i2s_channel_enable=-1
```

(`258` = `0x102` = `ESP_ERR_INVALID_ARG`, per this exact downloaded
package's `components/esp_common/include/esp_err.h` - not guessed. The
important part to check on a real boot is that
`i2s_channel_init_tdm_mode` is non-zero/non-`ESP_OK` and
`started=false`, confirming or contradicting the prediction above.)

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
Everything in this subsection is from the installed **headers only**:

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
  `i2s_driver_install()` alone, appeared to be the documented way to
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

**Attempted and REJECTED: the `i2s_set_clk()` adjustment did not change
the output.** Based on the header evidence above, an explicit
`i2s_set_clk(kI2sPort, kConfig.fsyncHz, bitsCfg, chan_mask_as_channel_t)`
call was added after `i2s_set_pin()` - not a guessed/invented
configuration, it requested the exact same `total_chan=16`/
`bits_per_sample=16` already set, via the driver's own dedicated clock
API. **A physical retest reflashed the generator twice with this change
and measured an identical result both times**:

```text
pclk_hz  ~= 1,024,100
fsync_hz ~= 16,003
ratio    ~= 63.99
```

Indistinguishable from the original measurement above. This call has
been **removed** from `src/dev/si3050_clock_probe_generator_main.cpp` -
it is not a fix, does not change the real signal, and must not be
reintroduced or presented as a solution without new evidence.

### Toolchain investigation: is the newer, native TDM driver available here?

Checked directly rather than assumed from online documentation:

- `esp_common/include/esp_idf_version.h` in this installed framework
  reports `ESP_IDF_VERSION_MAJOR=4`, `ESP_IDF_VERSION_MINOR=4`,
  `ESP_IDF_VERSION_PATCH=7` - this Arduino-ESP32 core
  (`framework-arduinoespressif32` 3.20017.241212+sha.dcc1105b) is built
  on **ESP-IDF 4.4.7**. The newer, better-specified I2S driver
  (`driver/i2s_std.h`, `driver/i2s_tdm.h`, `driver/i2s_pdm.h`, the
  `esp_driver_i2s` component) was introduced in **ESP-IDF 5.0** and does
  not exist in the 4.4.x line at all.
- A recursive search of every chip's SDK directory in this installed
  framework package (`tools/sdk/*/include/**`, all chips, not only
  esp32c3) for `i2s_tdm*`, `i2s_std*`, `i2s_pdm*`, `i2s_common*`, and any
  `esp_driver_i2s*` file found **none** - the only I2S driver header
  present anywhere in this package, for any chip, is the legacy
  `driver/i2s.h` already in use.
- `esp32-c3-si3050-clock-probe` therefore cannot compile even a minimal
  use of the newer driver in this framework version - there is nothing
  to `#include`.

**Conclusion: this is genuinely a toolchain/framework-version blocker,
not a vendoring omission or something fixable in firmware code alone.**
Reaching the newer, native TDM driver would require a newer
Arduino-ESP32 core release built on ESP-IDF >= 5.0 (if/when PlatformIO's
`espressif32` platform offers one) or building against ESP-IDF directly
- both toolchain decisions, out of this firmware change's scope. Per
explicit instruction, no further attempt was made with the legacy
driver, private/undocumented registers, LEDC, RMT, bit-banging, or delay
loops.

### What the generator now logs

Startup diagnostics report what was *requested*
(`requested_sample_rate_hz`, `requested_total_chan`,
`requested_bits_per_sample`, `requested_ratio`, `requested_pclk_hz`) and
whether the driver *accepted* that request (`started=true/false`, plus
each call's `esp_err_t` on failure) - never a frequency claim. The
`requested_ratio`/`requested_pclk_hz` values are computed via
`configuredTdmRatio()`/`configuredBclkHz()`
(`src/dev/si3050_clock_probe_generator_config.h`, natively tested) so the
log can never silently drift from the actual `total_chan`/`bits_per_sample`
values used to configure the driver. See "Expected output" below for the
exact lines.

### Minimal alternatives for a future decision (none implemented here)

Per explicit instruction not to implement a workaround/approximation:

1. **Move off the legacy compatibility driver.** Confirmed blocked for
   the Arduino-framework environments at this exact framework version
   (see above) - requires a newer Arduino-ESP32 core (ESP-IDF >= 5.0-
   based) or building against ESP-IDF directly. **Attempted as a
   follow-up, in a new, isolated `esp32-c3-si3050-clock-probe-idf5`
   environment** - see "IDF native TDM driver investigation" below. This
   did not simply resolve the mismatch: it surfaced a deeper, hardware-
   level constraint that applies regardless of driver.
2. **Reconsider the target geometry.** Decide whether the Si3050
   bring-up work actually requires this exact 2.048 MHz/8 kHz/256:1
   relationship from *this specific* ESP32-C3 peripheral on *this
   specific* toolchain, or whether an external clock generator/oscillator
   for the Si3050's PCM interface is the more realistic path for Rev A -
   a hardware decision, out of this firmware's scope.

**This PR stays as investigation and documentation.** It does not claim
the ESP32-C3 (on this toolchain) delivers the clock the Si3050 needs, and
it does not decide on an external oscillator or any other hardware
change - those remain open decisions for the team.

## IDF native TDM driver investigation

A third, isolated environment,
`esp32-c3-si3050-clock-probe-idf5`, attempts the exact same target
geometry (16 TDM slots, 16 bits/sample, PCM-short framing) using the
modern, native ESP-IDF I2S TDM driver (`driver/i2s_tdm.h`) instead of the
legacy `driver/i2s.h` the first attempt used. **It does not migrate
`esp32-c3`, `esp32-c3-dev-mqtt`, the meter, or anything else in this
repository off Arduino/ESP-IDF 4.4.7** - it is a separate, `framework =
espidf` PlatformIO environment that only affects this one bench entry
point.

### Confirmed toolchain (downloaded and inspected for real, not assumed)

- `~/.platformio/packages/framework-espidf/version.txt` and
  `components/esp_common/include/esp_idf_version.h`
  (`ESP_IDF_VERSION_MAJOR/MINOR/PATCH`) both confirm this package is
  **ESP-IDF 6.0.1** - not "5.x" as earlier (incorrect) documentation in
  this repository speculated before this package was actually
  downloaded.
- `components/esp_driver_i2s/include/driver/i2s_tdm.h` (plus
  `i2s_std.h`, `i2s_pdm.h`, `i2s_common.h`, and the `.c` implementation
  files) genuinely exist in this downloaded package - confirmed by
  listing the files, not by trusting online documentation.
- `components/soc/esp32c3/include/soc/soc_caps.h` reports
  `SOC_I2S_SUPPORTS_TDM=1` for ESP32-C3 in this package too, and
  `driver/i2s_tdm.h` itself is guarded by `#if SOC_I2S_SUPPORTS_TDM`, so
  it is genuinely compiled in for this target.
- A real build of `esp32-c3-si3050-clock-probe-idf5` (see "Verification"
  below) compiles and links successfully against this exact API -
  `pio run` for this environment is itself a minimal, real, reproducible
  use of the new driver, not just a header include check.

### Environment isolation: `build_src_filter` does not apply to `framework = espidf`

The first attempt to build this environment (before `src/CMakeLists.txt`
existed) failed loudly and informatively:

```text
Warning: the 'src_filter' option cannot be used with ESP-IDF. Select source files to build in the project CMakeLists.txt file.
...
src/aws/device_shadow.cpp:4:10: fatal error: ArduinoJson.h: No such file or directory
src/dev/mqtt_smoke_main.cpp:2:2: error: #error "mqtt_smoke_main.cpp is only for INTERBRIDGE_DEV_MQTT_SMOKE"
```

PlatformIO's `build_src_filter` (the isolation mechanism every other
bench environment in this file uses) has no effect for `framework =
espidf`: without an explicit `src/CMakeLists.txt`, PlatformIO
auto-generates one that globs and compiles *every* file under `src/` as
the "main" IDF component - including Arduino-only production files.
Fixed by adding `src/CMakeLists.txt` (new file, at the project source
root, alongside this shared `src/` directory) with an explicit
`idf_component_register(SRCS "dev/si3050_clock_probe_generator_idf5_main.cpp"
INCLUDE_DIRS ".")` - the ESP-IDF-native equivalent of `build_src_filter`
isolation. This file only takes effect for `framework = espidf` builds;
Arduino-framework environments (which use SCons, not CMake) and the
native test environment do not read it at all - confirmed by rebuilding
`esp32-c3`, `esp32-c3-dev-mqtt`, `esp32-c3-si3050-clock-probe`, and
`esp32dev-si3050-clock-meter` afterward and getting byte-identical binary
sizes to before this file existed (see "Verification" below).

### The critical finding: 16x16 exceeds this chip's TDM hardware limit, not just the legacy driver's

This was found in the downloaded package's own official example and its
own enforcing source code - not online documentation:

- `examples/peripherals/i2s/i2s_basic/i2s_tdm/main/i2s_tdm_example_main.c`
  states directly: *"For the target that not support full data
  bit-width in multiple slots (e.g. ESP32C3, ESP32S3, ESP32C6) ... the
  number of bit clock can't exceed 128 in one frame ... TDM mode can
  only support 32 bit-width data upto 4 slots, 16 bit-width data upto 8
  slots and 8 bit-width data upto 16 slots."*
- `components/esp_hal_i2s/esp32c3/include/hal/i2s_ll.h` defines
  `#define I2S_LL_SLOT_FRAME_BIT_MAX 128 // Up-to 128 bits in one frame`
  for ESP32-C3 specifically (other targets, e.g. esp32s3/esp32p4/esp32h2,
  define this as 512 - ESP32-C3's TDM hardware is genuinely more
  limited).
- `components/esp_driver_i2s/i2s_tdm.c`'s `i2s_channel_init_tdm_mode()`
  enforces this directly: `ESP_RETURN_ON_FALSE(handle->total_slot *
  slot_bits <= I2S_LL_SLOT_FRAME_BIT_MAX, ESP_ERR_INVALID_ARG, TAG,
  "total slots(%lu) * slot_bit_width(%lu) exceeds the maximum %d", ...)`.

**16 slots x 16 bits = 256 bits/frame is exactly 2x this chip's 128-bit
hardware ceiling.** This is a genuine ESP32-C3 silicon/TDM-hardware
limitation, confirmed from the actual enforcing driver source code, not
a legacy-driver quirk or a documentation gap - it would apply to *any*
driver on this exact chip, including the legacy one the first attempt
used (which likely explains that attempt's unpredictable ~64:1 result:
it was asking for a configuration the hardware cannot honor, and the
legacy driver has no equivalent validation to reject it cleanly).

### What was implemented

`src/dev/si3050_clock_probe_generator_idf5_main.cpp` - a dedicated
ESP-IDF `app_main()` entry point (not an adapted Arduino
`setup()`/`loop()`) that requests the **exact literal target geometry**
via the modern API, exactly as specified: `i2s_new_channel()` for a
master TX channel, then `i2s_channel_init_tdm_mode()` with
`I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
I2S_SLOT_MODE_MONO, <all 16 slots>)` and `total_slot=16` explicitly, BCLK
on GPIO0, WS on GPIO1, sample rate 8000 Hz - then `i2s_channel_enable()`.
`I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG` (`driver/i2s_tdm.h`) sets
`ws_width=1, ws_pol=true`: a single-BCLK-wide frame sync pulse, the
modern API's documented equivalent of the legacy driver's
`I2S_COMM_FORMAT_STAND_PCM_SHORT`, matching the Si3050's FSYNC
requirement the same way. Every call's `esp_err_t` is checked; nothing
is assumed to succeed.

**Given the finding above, `i2s_channel_init_tdm_mode()` is *predicted*
to return `ESP_ERR_INVALID_ARG`** - this is a prediction from reading the
exact enforcing source code, not yet confirmed by running on real
hardware in this session (no flashing was done - see "Verification"
below). The physical retest that will confirm or contradict this
prediction is left to the next bench session, per instruction.

Startup diagnostics follow the same pattern as the legacy probe: report
only what was *requested* (`requested_sample_rate_hz`,
`requested_total_slots`, `requested_bits_per_sample`, `requested_ratio`,
`requested_pclk_hz`, `started=true/false`, plus each call's `esp_err_t`
on failure) and never claim a real frequency - **only
`esp32dev-si3050-clock-meter`'s physical measurement validates actual
frequency**, for this environment exactly as for the legacy one. The
`requested_ratio`/`requested_pclk_hz` values reuse the same, already
natively-tested `configuredTdmRatio()`/`configuredBclkHz()`
(`src/dev/si3050_clock_probe_generator_config.h`) as the legacy
generator, rather than duplicating that arithmetic - no new pure math
was needed this round, so no new native tests were added.

### What was deliberately NOT done

- **No workaround for the 128-bit ceiling.** The realistic maximum
  achievable via TDM on ESP32-C3 (per the same source evidence) is a
  128-bit frame - e.g. 8 slots x 16 bits, or 4 slots x 32 bits (both
  ratio 128, not the target 256). Neither is implemented here; this PR
  configures and reports on the literal requested 16x16 geometry only,
  as specified, and leaves adopting a reduced-ratio alternative as an
  explicit decision for later, not something silently substituted in
  this firmware.
- **No decision on an external oscillator or any other hardware change.**
- **No touching of `esp32-c3-si3050-clock-probe` (the legacy Arduino
  probe)** beyond what earlier PRs already did - it remains in the repo
  only as a record of its own real measurement (~1.024 MHz/~16 kHz/~64:1)
  and is not presented as a solution.
- **No LEDC, RMT, bit-banging, delay-based approximation, or private/
  undocumented registers.**

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
`src/dev/si3050_clock_probe_generator_idf5_main.cpp` reuses these same
two functions (rather than duplicating the ratio/BCLK arithmetic a
second time) for its own `requested_ratio`/`requested_pclk_hz` logging -
no new pure math was introduced for the IDF5 investigation, so no new
native tests were needed for it.

Grep-verified (not itself a compiled test, since it is a structural/
negative property): neither `si3050_clock_probe_math.{h,cpp}`,
`si3050_clock_probe_generator_main.cpp`,
`si3050_clock_probe_generator_config.h`,
`si3050_clock_probe_generator_idf5_main.cpp`,
`si3050_clock_probe_meter_main.cpp`, nor
`si3050_clock_probe_meter_bringup.{h,cpp}` references Wi-Fi, MQTT,
`IHardwareIO`, `setDoorOutput`, or any door-actuation symbol. See the PR
description for the exact command and its (empty) result.

## Known limitations / not yet done

- **The meter's PCNT bring-up fix is confirmed on real hardware** (a
  physical retest with all three wires connected printed `pcnt
  configured` and reported a real, stable signal) - see "Real bench
  observation: PCNT bring-up order bug" above.
- **The generator does not reach the target PCLK/FSYNC/ratio on real
  hardware, confirmed by two separate physical retests** (the original
  TDM configuration, and an additional `i2s_set_clk()` adjustment that
  was flashed twice and measured an identical, still-wrong result both
  times - since removed from the code). The installed framework's I2S
  driver (ESP-IDF 4.4.7-based) is confirmed, not assumed, to lack the
  newer native TDM driver that might resolve this - see "Real bench
  observation: generator does not reach the target ratio" above,
  including the two alternatives left for a future team decision. This
  PR does not claim the ESP32-C3 delivers the Si3050's target clock on
  this toolchain.
- **The IDF native TDM driver attempt (`esp32-c3-si3050-clock-probe-idf5`)
  has NOT been flashed or tested on real hardware in this session** -
  only `pio run` (compile-only, confirmed real and successful) verifies
  it. Its `i2s_channel_init_tdm_mode()` call is *predicted* to fail with
  `ESP_ERR_INVALID_ARG`, based on reading this exact downloaded driver's
  own enforcing source code (`I2S_LL_SLOT_FRAME_BIT_MAX=128` for
  ESP32-C3), not on having run it - see "IDF native TDM driver
  investigation" above. The physical retest that will confirm or
  contradict this prediction is left for after review, per instruction.
  Only `esp32dev-si3050-clock-meter`'s physical measurement can validate
  any generator's real output - neither generator's own startup log
  proves anything about actual frequencies.
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
