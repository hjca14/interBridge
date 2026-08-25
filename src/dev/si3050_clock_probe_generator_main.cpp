#ifndef INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR
#error "si3050_clock_probe_generator_main.cpp is only for INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR"
#endif

// Phase 3B.1 bench experiment: generates the Si3050's PCM/SPI-mode
// PCLK/FSYNC clocks on GPIO0/GPIO1 using the ESP32-C3's I2S peripheral in
// hardware TDM master mode, measured by a second board running
// esp32dev-si3050-clock-meter. This is NOT Si3050 integration - it does
// not touch Si3050Controller, Esp32PcmClock (which remains an untouched,
// unintegrated stub - see below), or any production/DEV MQTT firmware
// path. No physical action of any kind is possible from this firmware.
// See docs/si3050-clock-probe.md.
//
// IMPORTANT - bench history:
// 1. The original configuration (total_chan=16, bits_per_sample=16,
//    requesting ratio 256, i.e. 2,048,000 Hz PCLK) measured an actual
//    PCLK:FSYNC ratio of ~64 (pclk_hz~=1,024,000, fsync_hz~=16,000) -
//    NOT the requested 256 - even though the meter confirmed a real,
//    stable signal was present.
// 2. An additional i2s_set_clk() call (requesting the exact same
//    total_chan/bits_per_sample via the driver's dedicated clock entry
//    point, as a documented-but-unproven adjustment) was tried and
//    RE-FLASHED TWICE - the measured result was identical
//    (pclk_hz~=1,024,100, fsync_hz~=16,003, ratio~=63.99) both times, so
//    it has been REMOVED - it is not a fix and must not be presented as
//    one, and must not be reintroduced without new evidence.
// 3. A source-grounded investigation (cross-referencing the matching
//    upstream espressif/esp-idf v4.4.7 tag's components/driver/i2s.c and
//    components/hal/i2s_hal.c, since this installed framework ships only
//    a precompiled libdriver.a) found that the driver's own documented
//    clock-divider formula does not predict the real measured output for
//    the 16 x 16 request - see docs/si3050-clock-probe.md's "Deeper
//    investigation" section.
//
// GEOMETRY CHANGE, PHYSICALLY VALIDATED: 16 slots x 8 bits = 128
// requested clocks/frame, instead of 16 x 16 = 256 - chosen because it
// directly matches the Si3050 datasheet's own PCM/SPI-mode PCM Highway
// description (16 timeslots x 8 bits/timeslot = 128 PCLK cycles/frame,
// FSYNC = 8 kHz, PCLK = 1.024 MHz - see docs/si3050-clock-probe.md's
// "Corrected premise"), not derived from the (already-shown-unreliable)
// clock formula above. **A physical retest with this exact geometry,
// flashed to a real ESP32-C3 and measured by the unchanged, already-
// validated esp32dev-si3050-clock-meter, confirmed `pclk_hz ~=
// 1,024,100`, `fsync_hz ~= 8,001`-`8,002`, `ratio ~= 127.98`-`128.00`
// across multiple stable reporting windows** (the first window
// immediately after boot showed a brief startup transient and is not
// representative - see docs/si3050-clock-probe.md's "Real bench
// observation: 16 x 8 slot geometry reaches the PCM/SPI target"). This
// confirms the ESP32-C3, on this exact toolchain/driver, can generate a
// PCM/SPI-mode-compatible Si3050 clock via this TDM configuration.
// **This confirms the clock signal only** - no real Si3050 has been
// connected or initialized, PCM DRX/DTX and audio are untested, and this
// validated configuration lives only in this isolated probe environment;
// it has not been integrated into `Esp32PcmClock` (still the untouched,
// unintegrated stub used by the real firmware path) or
// `Si3050Controller`.
//
// This installed framework (framework-arduinoespressif32
// 3.20017.241212+sha.dcc1105b) is built on ESP-IDF 4.4.7
// (esp_idf_version.h: ESP_IDF_VERSION_MAJOR=4, MINOR=4, PATCH=7) - the
// newer, better-specified I2S driver (driver/i2s_std.h,
// driver/i2s_tdm.h, driver/i2s_pdm.h) was introduced in ESP-IDF 5.0 and
// does not exist in IDF 4.4.x. A full search of every chip's SDK
// directory in this installed framework package
// (tools/sdk/*/include/**) confirms none of those headers - or any
// `esp_driver_i2s` component - are present anywhere; only the legacy
// `driver/i2s.h` compatibility driver used below exists. This is a
// genuine toolchain/framework-version blocker, not a vendoring
// omission: reaching the newer driver would require a newer
// Arduino-ESP32 core release (IDF 5.x-based) or building against
// ESP-IDF directly, either of which is a toolchain decision, not a
// firmware code change - see docs/si3050-clock-probe.md's "Real bench
// observation: generator does not reach the target ratio".
//
// Per explicit instruction, no further attempt was made with the legacy
// driver's i2s_set_clk(), private/undocumented registers, LEDC, RMT,
// bit-banging, or delay loops.

#include <Arduino.h>
#include "driver/i2s.h"

#include "../intercom/si3050/si3050_config.h"
#include "../intercom/si3050/si3050_pins.h"
#include "si3050_clock_probe_generator_config.h"

using namespace interbridge;

namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;

// TDM slot geometry REQUESTED from the driver, and PHYSICALLY CONFIRMED
// by a real bench retest to be honored (see the file-level comment
// above and docs/si3050-clock-probe.md's "Real bench observation: 16 x 8
// slot geometry reaches the PCM/SPI target") - unlike the previous
// 16 x 16 geometry, which real bench tests showed was NOT honored as
// documented.
//   requested BCLK (PCLK) = sample_rate * total_chan * bits_per_sample
//                          = 8000 * 16 * 8 = 1,024,000 Hz
//   requested WS (FSYNC)  = sample_rate = 8000 Hz
//   requested ratio       = total_chan * bits_per_sample = 128
//
// TDM channel activation is a bitmask of I2S_TDM_ACTIVE_CH0..CH15
// (hal/i2s_types.h), i.e. a hard ceiling of 16 channels exposed by this
// driver. 16 channels x 8 bits was chosen deliberately (not derived from
// the driver's own clock formula, already shown unreliable for the
// previous geometry - see the file-level comment above) because it is
// exactly the Si3050 datasheet's own PCM/SPI-mode PCM Highway geometry:
// 16 timeslots of 8 bits each, 128 PCLK cycles/frame.
// `I2S_BITS_PER_SAMPLE_8BIT` (hal/i2s_types.h) is a directly-supported
// value of `i2s_bits_per_sample_t` on this chip - confirmed in the
// installed header, not assumed - and the driver's own
// `i2s_driver_install()` validation only requires
// `bits_per_sample % 8 == 0 && bits_per_sample <= 32`, which 8 satisfies.
// `bits_per_chan` is left at its default (0 = "equal to bits_per_sample"
// per the same header), so the TDM channel width matches
// `bits_per_sample` exactly - there is no 16-bit assumption left
// anywhere else in this configuration (DMA buffer sizing below is
// computed from the same 8-bit width).
// `I2S_COMM_FORMAT_STAND_PCM_SHORT` is documented in hal/i2s_types.h as
// "PCM Short standard, also known as DSP mode. The period of
// synchronization signal (WS) is 1 bck cycle" - the short, single-cycle
// frame pulse the Si3050 datasheet calls FSYNC, not I2S's own ~50%-duty
// Philips WS. No approximation (LEDC/RMT/bit-banged delay loop) is used.
constexpr uint32_t kTotalChannels = 16;
constexpr i2s_bits_per_sample_t kBitsPerSample = I2S_BITS_PER_SAMPLE_8BIT;

// All 16 TDM slots active. Only their *timing* matters for this probe -
// their data content is left as zero-filled silence (see setup()).
constexpr uint32_t kActiveChannelMask = I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1 | I2S_TDM_ACTIVE_CH2 |
                                        I2S_TDM_ACTIVE_CH3 | I2S_TDM_ACTIVE_CH4 | I2S_TDM_ACTIVE_CH5 |
                                        I2S_TDM_ACTIVE_CH6 | I2S_TDM_ACTIVE_CH7 | I2S_TDM_ACTIVE_CH8 |
                                        I2S_TDM_ACTIVE_CH9 | I2S_TDM_ACTIVE_CH10 | I2S_TDM_ACTIVE_CH11 |
                                        I2S_TDM_ACTIVE_CH12 | I2S_TDM_ACTIVE_CH13 | I2S_TDM_ACTIVE_CH14 |
                                        I2S_TDM_ACTIVE_CH15;

const Si3050Config kConfig; // fsyncHz=8000 reused from here, not duplicated - kConfig.pclkHz (its Rev A
                            // default, unrelated to this probe's own physically-confirmed PCLK request
                            // below) is not read by this file; the requested PCLK comes from
                            // configuredBclkHz() instead.

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + 3000;
    while (!Serial && !deadlineReached(millis(), serialDeadline)) delay(10);

    i2s_config_t i2sConfig = {};
    i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    i2sConfig.sample_rate = kConfig.fsyncHz;
    i2sConfig.bits_per_sample = kBitsPerSample;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_MULTIPLE; // TDM
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT; // 1-BCK-wide WS/FSYNC pulse
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 64; // 64 frames * 16 chan * 8 bit / 8 = 1024 bytes/buffer, under the driver's 4092-byte limit
    i2sConfig.use_apll = false;
    i2sConfig.tx_desc_auto_clear = true; // keep BCLK/WS toggling with silence if the (unused) TX data ever underflows
    i2sConfig.fixed_mclk = 0;
    i2sConfig.chan_mask = static_cast<i2s_channel_t>(kActiveChannelMask);
    i2sConfig.total_chan = kTotalChannels;
    i2sConfig.left_align = false;
    i2sConfig.big_edin = false;
    i2sConfig.bit_order_msb = false;
    i2sConfig.skip_msk = false;

    const esp_err_t installResult = i2s_driver_install(kI2sPort, &i2sConfig, 0, nullptr);

    i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
    pinConfig.bck_io_num = kSi3050PinPclk;  // GPIO0 - PCLK
    pinConfig.ws_io_num = kSi3050PinFsync;  // GPIO1 - FSYNC
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE; // data content is irrelevant to this probe
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

    const esp_err_t pinResult = (installResult == ESP_OK) ? i2s_set_pin(kI2sPort, &pinConfig) : installResult;

    // NOTE: an additional i2s_set_clk() call was tried here (requesting
    // the same total_chan/bits_per_sample via the driver's dedicated
    // clock-configuration entry point) and removed after two real bench
    // reflashes measured an identical, still-wrong ratio - see the
    // file-level comment above. Do not re-add it without new evidence
    // it would actually change the output.

    const bool started = (installResult == ESP_OK) && (pinResult == ESP_OK);
    if (started) {
        i2s_zero_dma_buffer(kI2sPort); // fill DMA with silence so BCLK/WS run continuously from the start
    }

    // requestedClocksPerFrame is the number of PCLK cycles the driver is
    // asked to place between consecutive FSYNC pulses (slot_count *
    // slot_width_bits) - the same quantity configuredTdmRatio() always
    // computed, named explicitly here so a log reader never has to infer
    // "ratio" means "clocks per frame". None of these fields are a
    // frequency claim - see the note line below.
    const uint32_t requestedClocksPerFrame = configuredTdmRatio(kTotalChannels, static_cast<uint32_t>(kBitsPerSample));
    const uint32_t requestedPclkHz =
        configuredBclkHz(kConfig.fsyncHz, kTotalChannels, static_cast<uint32_t>(kBitsPerSample));
    Serial.printf(
        "[SI3050 CLOCK PROBE] requested_sample_rate_hz=%lu requested_fsync_hz=%lu requested_pclk_hz=%lu "
        "requested_clocks_per_frame=%lu slot_count=%lu slot_width_bits=%lu started=%s\n",
        static_cast<unsigned long>(kConfig.fsyncHz), static_cast<unsigned long>(kConfig.fsyncHz),
        static_cast<unsigned long>(requestedPclkHz), static_cast<unsigned long>(requestedClocksPerFrame),
        static_cast<unsigned long>(kTotalChannels), static_cast<unsigned long>(kBitsPerSample),
        started ? "true" : "false");
    Serial.println(
        "[SI3050 CLOCK PROBE] note: the line above reports what was requested from the I2S driver, not a "
        "measurement - only esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies. "
        "A real bench retest of this exact 16x8 geometry confirmed it reaches the PCM/SPI target "
        "(~1.024 MHz/~8 kHz/~128) - see docs/si3050-clock-probe.md");
    if (!started) {
        Serial.printf("[SI3050 CLOCK PROBE] i2s_driver_install=%d i2s_set_pin=%d\n", static_cast<int>(installResult),
                     static_cast<int>(pinResult));
    }
}

void loop() {
    // The I2S peripheral generates PCLK/FSYNC entirely in hardware from
    // here on; there is nothing for this loop to drive. A slow idle
    // delay is fine here - it is not standing in for the clock itself.
    delay(1000);
}
