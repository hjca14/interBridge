# Si3050 Clock Probe: ESP32-C3 -> ESP32 DevKitV1 (Phase 3B.1)

This document describes a bench-only experiment to validate two narrow
questions before any real Si3050 hardware exists.

## Corrected premise: PCM/SPI mode, not GCI mode

Earlier versions of this document (and of PR #16) treated `PCLK =
2.048 MHz`, `FSYNC = 8 kHz`, `ratio = 256` as *the* mandatory Si3050
clock target. **That premise was wrong.** A full read of the Si3050
datasheet's Clock Generation and PCM Highway sections shows the part has
two distinct communication interface modes (Section 5.31,
"Communication Interface Mode Selection"), selected by the level of
SCLK at the instant `/RESET` is sampled:

- **PCM/SPI mode - the mode InterBridge plans to use** (SPI for
  control, separate from PCM for audio, per `si3050-bringup.md`'s
  Section 5.31 citation - SCLK held high before `/RESET` release selects
  this mode): PCLK must be synchronous to an 8 kHz FSYNC, and
  **1.024 MHz is a valid PCLK rate in this mode**
  (1,024,000 / 8,000 = 128 PCLK cycles per frame = 16 timeslots of 8
  bits). `2.048 MHz` is **not** a requirement of this mode.
- **GCI mode - not the mode InterBridge plans to use**: control and
  data are multiplexed on the same highway, and this is the mode that
  requires `PCLK = 2.048` or `4.096 MHz`.

Because InterBridge uses SPI for control and PCM for audio (PCM/SPI
mode), the corrected target for this probe is **`PCLK ~= 1.024 MHz`,
`FSYNC ~= 8 kHz`, `ratio ~= 128`** - not 2.048 MHz/256. See "Real bench
observation: generator does not reach the target ratio" below: the
existing generator's real bench measurement of ~1.024 MHz PCLK was
*closer* to this corrected target than anyone realized at the time, and
the 256-ratio framing throughout the rest of this document (kept
verbatim below as the historical record of what was actually measured
and concluded in PR #14-#16) should be read against this corrected
target instead. This document's own conclusion at the time - "the
generator does not reach the target ratio" - remains materially true
either way (a measured ~64:1 or ~128:1 is not yet a confirmed match to
either target), but the *reason* it looked so far off (a 4x gap to 256)
was partly an artifact of comparing against the wrong mode's numbers.

This document's original two questions, as originally framed:

1. Can an ESP32-C3 generate the Si3050's target PCM clocks (originally
   stated here as PCLK = 2.048 MHz, FSYNC = 8 kHz, ratio 256 - see the
   correction above) in hardware? **Not confirmed yet, even against the
   corrected target.** Two real bench tests (the original TDM
   configuration, then an additional `i2s_set_clk()` adjustment)
   measured an identical ~1.024 MHz/~16 kHz/~64:1 output, and the
   framework version installed here does not vendor the newer,
   better-specified I2S driver that might do better - see "Real bench
   observation: generator does not reach the target ratio" below for the
   full investigation and the alternatives left for a future decision.
   Critically, that ~16 kHz FSYNC reading has **not yet been retested**
   against the corrected meter (see "Real bench observation: meter edge
   configuration re-examined" below) - a fresh physical retest is needed
   before drawing any conclusion about how close the generator actually
   is to the corrected 1.024 MHz/8 kHz/128 target.
2. Can a second board measure those clocks by hardware pulse counting and
   report believable frequencies/ratio back over serial? **Yes** - a real
   bench retest confirmed the PCNT meter (after its own bring-up fix,
   see "Real bench observation: PCNT bring-up order bug" below) reports a
   real, stable signal and ratio, even though that ratio has not yet been
   confirmed to match either target.

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
  (`PCNT_COUNT_INC`/`PCNT_COUNT_DIS`), no control-signal gating. Both
  units are configured by the *same* `configurePcntUnit()` function with
  the same `pos_mode`/`neg_mode` values, so PCLK and FSYNC are
  guaranteed to use identical, rising-edge-only counting - see "Real
  bench observation: meter edge configuration re-examined" below for why
  this matters and how it was confirmed by code inspection (not assumed).
  The reported `pclk_rising_edges`/`fsync_rising_edges` fields name this
  explicitly, so a rising-edge count is never confused with a
  both-edges/transition count.
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
measurement** of the current generator configuration, from *before* the
edge-naming/premise correction below (see "Real bench observation: meter
edge configuration re-examined" for why this exact reading still needs a
fresh retest, and "Corrected premise" above for the corrected target):

```text
[SI3050 CLOCK METER] pcnt configured pclk_pin=34 fsync_pin=35 h_lim=30000
[SI3050 CLOCK METER] pclk_edge_mode=rising_only fsync_edge_mode=rising_only
[SI3050 CLOCK METER] window_us=1000091 pclk_rising_edges=1024145 pclk_hz=1024082.6 fsync_rising_edges=16003 fsync_hz=16001.5 ratio=63.996
[SI3050 CLOCK METER] stats pclk_hz_min=1023991.0 pclk_hz_max=1024211.0 fsync_hz_min=15999.3 fsync_hz_max=16006.8 ratio_min=63.993 ratio_max=64.001
```

(The field names above - `pclk_rising_edges`/`fsync_rising_edges` - are
what the meter logs *after* this PR's renaming; the underlying numbers
shown are the real bench values from the retest described below, which
predates the rename and was captured under the old `pclk_edges`/
`fsync_edges` names.)

The corrected target this probe is actually checking against is `PCLK ~=
1,024,000 Hz`, `FSYNC ~= 8,000 Hz`, `ratio ~= 128` (PCM/SPI mode - see
"Corrected premise" above), not 2,048,000 Hz/8,000 Hz/256. The measured
`pclk_hz` above (~1,024,000) already lands close to that corrected PCLK
target; `fsync_hz` (~16,000) and `ratio` (~64) do not yet match it.
**Clock compatibility with the corrected PCM/SPI target should only be
marked as physically confirmed once a new bench test - with the meter's
edge configuration re-examined as below - shows approximately `pclk_hz
~= 1,024,000`, `fsync_hz ~= 8,000`, `ratio ~= 128`.** Real crystal/clock
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

## Real bench observation: meter edge configuration re-examined

After the "Corrected premise" section above, one hypothesis was raised:
maybe the ~16 kHz FSYNC reading in the retest below is a meter artifact
- the PCNT counting *both* edges of FSYNC while counting only *one* edge
of PCLK, which would make a true 8 kHz FSYNC read out as ~16 kHz.

**Checked by reading the code, not assumed.** In
`src/dev/si3050_clock_probe_meter_main.cpp`, both the PCLK unit
(`kPclkUnit`) and the FSYNC unit (`kFsyncUnit`) are configured by the
*same* `configurePcntUnit()` function, which sets `pos_mode =
PCNT_COUNT_INC` (count rising edges) and `neg_mode = PCNT_COUNT_DIS`
(ignore falling edges) unconditionally - there is no separate code path
or parameter that could make one unit count both edges while the other
counts one. **Both PCLK and FSYNC were already configured identically,
rising-edges-only, before this PR.** The double-edge-counting hypothesis
is therefore not what the code shows; this PR does not change that
configuration.

What this PR does change is naming only: the log fields are renamed
`pclk_edges`/`fsync_edges` -> `pclk_rising_edges`/`fsync_rising_edges`
(and the equivalent `ClockProbeWindowResult` struct fields in
`src/dev/si3050_clock_probe_math.h`) so a rising-edge count can never be
misread as a both-edges count in the first place, and a new boot-time
line (`pclk_edge_mode=rising_only fsync_edge_mode=rising_only`) states
the configuration explicitly without claiming any frequency has been
validated.

**The ~16 kHz FSYNC reading in the retest below therefore remains
unexplained by the meter and is not corrected by this PR.** Since the
meter counts rising edges only on both signals, a genuinely
rising-edge-only ~16 kHz reading on a signal expected to be ~8 kHz is a
real, physical discrepancy against the corrected PCM/SPI-mode target
(see "Corrected premise" above) - not a counting artifact. Resolving it
is generator-side investigation, explicitly out of scope for this PR
(see "Generator" in the PR description / this document's isolation
section) - it is left for a follow-up bench session. **A fresh physical
retest is still needed** to (a) confirm the corrected target
understanding against real hardware, and (b) determine whether the
generator's actual output is closer to the PCM/SPI target than the old
2.048 MHz/256 framing made it look.

## Real bench observation: generator does not reach the target ratio

**Read against the corrected target from "Corrected premise" above,
not the original one.** Everything below this point is the unmodified
historical record of what was measured and concluded at the time
(against the then-assumed `2.048 MHz`/`256` GCI-style target) - it is
kept verbatim rather than rewritten, but its `requested_pclk_hz=
2,048,000`/`ratio=256` framing reflects the generator's own
still-unchanged request (this PR does not touch the generator - see
"Generator" below), not a confirmed Si3050 requirement. The generator
was never reconfigured for the corrected PCM/SPI target
(1.024 MHz/8 kHz/128) in this PR, and per "Real bench observation: meter
edge configuration re-examined" above, the ~16 kHz FSYNC figure below
has not yet been re-verified with a fresh physical retest.

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

1. **Move off the legacy compatibility driver.** Confirmed blocked at
   this exact framework version (see above) - requires a newer
   Arduino-ESP32 core (ESP-IDF >= 5.0-based) or building against ESP-IDF
   directly. A toolchain/framework version decision, not a firmware code
   change.
2. **Reconsider the target geometry.** *Partially resolved by "Corrected
   premise" above*: the Si3050 does **not** require exactly 2.048 MHz/
   8 kHz/256:1 for the PCM/SPI mode InterBridge plans to use - 1.024 MHz/
   8 kHz/128:1 is a valid PCM/SPI-mode target per the datasheet. What
   remains open is whether *this specific* ESP32-C3 I2S peripheral on
   *this specific* toolchain can reliably deliver 1.024 MHz/8 kHz/128:1
   (still unconfirmed - see "Real bench observation: meter edge
   configuration re-examined" above), or whether an external clock
   generator/oscillator for the Si3050's PCM interface is the more
   realistic path for Rev A - a hardware decision, out of this firmware's
   scope.

**This PR stays as investigation and documentation.** It does not claim
the ESP32-C3 (on this toolchain) delivers the clock the Si3050 needs, and
it does not decide on an external oscillator or any other hardware
change - those remain open decisions for the team.

## Tests

`test/test_si3050_clock_probe_math/test_main.cpp` (7 tests, native, no
hardware): the corrected PCM/SPI-mode target ratio (128, via the new
`kPcmSpiTargetPclkHz`/`kPcmSpiTargetFsyncHz`/`kPcmSpiTargetRatio`
constants in `si3050_clock_probe_math.h` - see "Corrected premise"
above) from raw rising-edge counts; a full `ClockProbeWindowResult` at
that target's frequencies (asserted against the renamed
`pclkRisingEdges`/`fsyncRisingEdges` fields); frequency conversion using
a real (non-1-second) window duration; zero/invalid counts and windows
never dividing by zero; overflow-cycle combination plus an out-of-range
raw count treated as invalid rather than producing a huge or wrong
total; and the min/max tracker (including that non-finite samples are
ignored). `TEST_ASSERT_EQUAL_DOUBLE` required adding
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
nothing about what the hardware actually produces, and the generator
itself is unchanged by this PR (see "Generator" above); `ratio=256`
here is still the generator's own unmodified TDM request, not a claim
about the Si3050's actual requirement - see "Corrected premise" and
"Real bench observation: generator does not reach the target ratio"
above.

Grep-verified (not itself a compiled test, since it is a structural/
negative property): neither `si3050_clock_probe_math.{h,cpp}`,
`si3050_clock_probe_generator_main.cpp`,
`si3050_clock_probe_generator_config.h`,
`si3050_clock_probe_meter_main.cpp`, nor
`si3050_clock_probe_meter_bringup.{h,cpp}` references Wi-Fi, MQTT,
`IHardwareIO`, `setDoorOutput`, or any door-actuation symbol. See the PR
description for the exact command and its (empty) result.

## Known limitations / not yet done

- **The clock target used throughout most of this document (below this
  point) was corrected in this PR** - see "Corrected premise" at the top.
  InterBridge plans to use PCM/SPI mode (SPI for control, PCM for
  audio), whose valid target is `PCLK ~= 1.024 MHz`/`FSYNC ~= 8 kHz`/
  `ratio ~= 128`, not the `2.048 MHz`/`256` GCI-mode figure this document
  originally treated as mandatory.
- **No real Si3050 hardware initialization was performed in this PR or
  any prior one.** This probe only exercises an ESP32-C3 I2S peripheral
  and an ESP32 DevKitV1's PCNT peripheral against each other - no Si3050
  or Si3011/18/19 part has been connected, initialized, or read from at
  any point in this experiment.
- **The ~16 kHz FSYNC reading below has not been retested since the
  meter edge-configuration re-examination in this PR.** The meter's
  PCNT edge configuration itself was not changed (see "Real bench
  observation: meter edge configuration re-examined" above) - only its
  log field names were - so a fresh physical retest is needed before
  treating any pclk/fsync/ratio number in this document as confirmed
  against the corrected PCM/SPI target. **Clock compatibility should
  only be marked as physically confirmed once that retest shows
  approximately `pclk_hz ~= 1,024,000`, `fsync_hz ~= 8,000`, `ratio ~=
  128`.**
- **The generator was not changed in this PR** and still requests the
  original TDM geometry (`total_chan=16`, `bits_per_sample=16`,
  logged as `requested_ratio=256`/`requested_pclk_hz=2048000`) - see
  "Generator" above. Whether to change the generator's requested
  geometry to target 1.024 MHz/128:1 directly is a follow-up decision,
  out of scope here.
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
