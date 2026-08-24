#ifndef INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR
#error "si3050_clock_probe_generator_main.cpp is only for INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR"
#endif

// Phase 3B.1 bench-only experiment: generates the Si3050's target PCLK/
// FSYNC clocks on GPIO0/GPIO1 using the ESP32-C3's I2S peripheral in
// hardware TDM master mode, to be measured by a second board running
// esp32dev-si3050-clock-meter. This is NOT Si3050 integration - it does
// not touch Si3050Controller, Esp32PcmClock (which remains an untouched
// stub), or any production/DEV MQTT firmware path. No physical action of
// any kind is possible from this firmware. See
// docs/si3050-clock-probe.md.

#include <Arduino.h>
#include "driver/i2s.h"

#include "../intercom/si3050/si3050_config.h"
#include "../intercom/si3050/si3050_pins.h"

using namespace interbridge;

namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;

// TDM slot geometry chosen so that, with the Si3050Config defaults
// (pclkHz=2048000, fsyncHz=8000, reused below rather than duplicated as
// fresh magic numbers):
//   PCLK (I2S BCLK) = sample_rate * total_chan * bits_per_sample
//                    = 8000 * 16 * 16 = 2,048,000 Hz
//   FSYNC (I2S WS)  = sample_rate = 8000 Hz
//   ratio           = total_chan * bits_per_sample = 256 PCLK cycles/frame
// matching the Si3050's targets exactly.
//
// Confirmed against the framework actually installed in this repo
// (framework-arduinoespressif32 3.20017.241212+sha.dcc1105b, ESP-IDF 5.x
// legacy `driver/i2s.h`; soc_caps.h reports SOC_I2S_SUPPORTS_TDM=1 for
// ESP32-C3):
//   - `i2s_channel_fmt_t::I2S_CHANNEL_FMT_MULTIPLE` selects TDM mode.
//   - TDM channel activation is a bitmask of I2S_TDM_ACTIVE_CH0..CH15
//     (hal/i2s_types.h), i.e. a hard ceiling of 16 channels - so 16
//     channels x 16 bits is the widest TDM slot geometry that reaches
//     exactly 256 without exceeding that ceiling (32 x 8 or 8 x 32 would
//     also reach 256 arithmetically, but 32 channels exceeds the 16-slot
//     TDM bitmask this driver exposes).
//   - `I2S_COMM_FORMAT_STAND_PCM_SHORT` is documented in
//     hal/i2s_types.h as "PCM Short standard, also known as DSP mode.
//     The period of synchronization signal (WS) is 1 bck cycle" - this
//     is the short, single-cycle frame pulse the Si3050 datasheet calls
//     FSYNC, not I2S's own ~50%-duty Philips WS.
// No approximation (LEDC/RMT/bit-banged delay loop) was used or needed.
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
    const bool started = (installResult == ESP_OK) && (pinResult == ESP_OK);
    if (started) {
        i2s_zero_dma_buffer(kI2sPort); // fill DMA with silence so BCLK/WS run continuously from the start
    }

    const uint32_t ratioTarget = kTotalChannels * static_cast<uint32_t>(kBitsPerSample);
    Serial.printf("[SI3050 CLOCK PROBE] pclk_target_hz=%lu fsync_target_hz=%lu ratio_target=%lu started=%s\n",
                 static_cast<unsigned long>(kConfig.pclkHz), static_cast<unsigned long>(kConfig.fsyncHz),
                 static_cast<unsigned long>(ratioTarget), started ? "true" : "false");
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
