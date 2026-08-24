#ifndef INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR
#error "si3050_clock_probe_generator_main.cpp is only for INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR"
#endif

// Phase 3B.1 bench-only experiment: attempts to generate the Si3050's
// target PCLK/FSYNC clocks on GPIO0/GPIO1 using the ESP32-C3's I2S
// peripheral in hardware TDM master mode, to be measured by a second
// board running esp32dev-si3050-clock-meter. This is NOT Si3050
// integration - it does not touch Si3050Controller, Esp32PcmClock
// (which remains an untouched stub), or any production/DEV MQTT
// firmware path. No physical action of any kind is possible from this
// firmware. See docs/si3050-clock-probe.md.
//
// IMPORTANT: a real bench retest of the original configuration below
// (i2s_driver_install() + i2s_set_pin() only, total_chan=16,
// bits_per_sample=16, expected ratio 256) measured an actual PCLK:FSYNC
// ratio of ~64 - NOT the requested 256 - even though pcnt confirmed a
// real, stable signal was present. This firmware does NOT claim the
// values it logs at startup are the real output frequencies - only the
// separate meter board's measurement is that. See
// docs/si3050-clock-probe.md's "Real bench observation: generator does
// not reach the target ratio" for the full investigation, what was
// tried here as a documented, driver-justified adjustment
// (i2s_set_clk() below), and why it is still unconfirmed pending a
// fresh bench retest.

#include <Arduino.h>
#include "driver/i2s.h"

#include "../intercom/si3050/si3050_config.h"
#include "../intercom/si3050/si3050_pins.h"
#include "si3050_clock_probe_generator_config.h"

using namespace interbridge;

namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;

// TDM slot geometry REQUESTED from the driver - see the file-level
// comment above: a real bench test showed this request is NOT honored
// as documented by this driver/chip/framework combination, so this is
// no longer described as "matching the target exactly" the way it was
// before that test - only as what is asked for.
//   requested BCLK (PCLK) = sample_rate * total_chan * bits_per_sample
//                          = 8000 * 16 * 16 = 2,048,000 Hz
//   requested WS (FSYNC)  = sample_rate = 8000 Hz
//   requested ratio       = total_chan * bits_per_sample = 256
//
// TDM channel activation is a bitmask of I2S_TDM_ACTIVE_CH0..CH15
// (hal/i2s_types.h), i.e. a hard ceiling of 16 channels exposed by this
// driver - so 16 channels x 16 bits was chosen as the widest TDM slot
// geometry that reaches exactly 256 without exceeding that ceiling.
// `I2S_COMM_FORMAT_STAND_PCM_SHORT` is documented in hal/i2s_types.h as
// "PCM Short standard, also known as DSP mode. The period of
// synchronization signal (WS) is 1 bck cycle" - the short, single-cycle
// frame pulse the Si3050 datasheet calls FSYNC, not I2S's own ~50%-duty
// Philips WS. No approximation (LEDC/RMT/bit-banged delay loop) is used.
constexpr uint32_t kTotalChannels = 16;
constexpr i2s_bits_per_sample_t kBitsPerSample = I2S_BITS_PER_SAMPLE_16BIT;

// All 16 TDM slots active. Only their *timing* matters for this probe -
// their data content is left as zero-filled silence (see setup()).
constexpr uint32_t kActiveChannelMask = I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1 | I2S_TDM_ACTIVE_CH2 |
                                        I2S_TDM_ACTIVE_CH3 | I2S_TDM_ACTIVE_CH4 | I2S_TDM_ACTIVE_CH5 |
                                        I2S_TDM_ACTIVE_CH6 | I2S_TDM_ACTIVE_CH7 | I2S_TDM_ACTIVE_CH8 |
                                        I2S_TDM_ACTIVE_CH9 | I2S_TDM_ACTIVE_CH10 | I2S_TDM_ACTIVE_CH11 |
                                        I2S_TDM_ACTIVE_CH12 | I2S_TDM_ACTIVE_CH13 | I2S_TDM_ACTIVE_CH14 |
                                        I2S_TDM_ACTIVE_CH15;

const Si3050Config kConfig; // pclkHz=2048000, fsyncHz=8000 - reused, not duplicated.

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
    i2sConfig.dma_buf_len = 64; // 64 frames * 16 chan * 16 bit / 8 = 2048 bytes/buffer, under the driver's 4092-byte limit
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

    // Driver-documented, justified adjustment attempted in response to
    // the real-hardware ratio mismatch: `driver/i2s.h`'s i2s_hal layer
    // exposes a distinct `active_chan` (I2S active channel number) from
    // `total_chan` (i2s_hal.h); the legacy i2s_config_t used by
    // i2s_driver_install() above only sets total_chan/chan_mask, with no
    // direct way to set active_chan. i2s_set_clk()'s own doc comment
    // documents a `ch` parameter explicitly described as accepting
    // "I2S_CHANNEL_MONO, I2S_CHANNEL_STEREO or specific channel in TDM
    // mode" - i.e. it is the documented way to (re)apply the TDM active-
    // channel bitmask specifically for clock configuration, which
    // i2s_driver_install() alone may not fully resolve for TDM. This is
    // NOT a new/guessed configuration - it requests the exact same
    // total_chan/bits_per_sample already set above, just through the
    // driver's own dedicated clock-configuration entry point.
    // UNCONFIRMED: this has not been re-verified on real hardware in
    // this session - see docs/si3050-clock-probe.md.
    const uint32_t bitsCfg = static_cast<uint32_t>(kBitsPerSample); // low 16 bits only -> chan_bits defaults to match
    const esp_err_t clkResult = (pinResult == ESP_OK)
                                     ? i2s_set_clk(kI2sPort, kConfig.fsyncHz, bitsCfg,
                                                   static_cast<i2s_channel_t>(kActiveChannelMask))
                                     : pinResult;

    const bool started = (installResult == ESP_OK) && (pinResult == ESP_OK) && (clkResult == ESP_OK);
    if (started) {
        i2s_zero_dma_buffer(kI2sPort); // fill DMA with silence so BCLK/WS run continuously from the start
    }

    const uint32_t requestedRatio = configuredTdmRatio(kTotalChannels, static_cast<uint32_t>(kBitsPerSample));
    const uint32_t requestedPclkHz = configuredBclkHz(kConfig.fsyncHz, kTotalChannels, static_cast<uint32_t>(kBitsPerSample));
    Serial.printf(
        "[SI3050 CLOCK PROBE] requested_sample_rate_hz=%lu requested_total_chan=%lu requested_bits_per_sample=%lu "
        "requested_ratio=%lu requested_pclk_hz=%lu started=%s\n",
        static_cast<unsigned long>(kConfig.fsyncHz), static_cast<unsigned long>(kTotalChannels),
        static_cast<unsigned long>(kBitsPerSample), static_cast<unsigned long>(requestedRatio),
        static_cast<unsigned long>(requestedPclkHz), started ? "true" : "false");
    Serial.println(
        "[SI3050 CLOCK PROBE] note: the line above reports what was requested from the I2S driver, not a "
        "measurement - only esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies");
    if (!started) {
        Serial.printf("[SI3050 CLOCK PROBE] i2s_driver_install=%d i2s_set_pin=%d i2s_set_clk=%d\n",
                     static_cast<int>(installResult), static_cast<int>(pinResult), static_cast<int>(clkResult));
    }
}

void loop() {
    // The I2S peripheral generates PCLK/FSYNC entirely in hardware from
    // here on; there is nothing for this loop to drive. A slow idle
    // delay is fine here - it is not standing in for the clock itself.
    delay(1000);
}
