#ifndef INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR_IDF5
#error "si3050_clock_probe_generator_idf5_main.cpp is only for INTERBRIDGE_SI3050_CLOCK_PROBE_GENERATOR_IDF5"
#endif

// Phase 3B.1 continuation: attempts the same PCLK/FSYNC generation as
// esp32-c3-si3050-clock-probe (src/dev/si3050_clock_probe_generator_main.cpp),
// but using the modern, native ESP-IDF I2S TDM driver (driver/i2s_tdm.h)
// instead of the legacy driver/i2s.h - see
// docs/si3050-clock-probe.md's "IDF5 native TDM driver investigation".
// This is a dedicated ESP-IDF app_main() entry point, NOT an adaptation
// of Arduino setup()/loop() - it does not include Arduino.h and cannot
// be built by any Arduino-framework environment (build_src_filter
// excludes it everywhere else; framework = espidf here uses
// src/CMakeLists.txt instead, see that file). This is NOT Si3050
// integration: no Si3050Controller, no SPI/RGDT/reset, no Wi-Fi/MQTT, no
// physical action of any kind. The legacy Arduino probe
// (esp32-c3-si3050-clock-probe) is left untouched and remains only as a
// record of its own real-hardware result (~1.024 MHz/~16 kHz/~64:1, not
// the target) - not a solution.
//
// IMPORTANT - read before assuming this reaches the target ratio:
// the ESP32-C3's I2S TDM hardware has a documented, SOURCE-CONFIRMED
// maximum frame width of 128 bits (`I2S_LL_SLOT_FRAME_BIT_MAX` = 128 in
// this exact downloaded framework's
// components/esp_hal_i2s/esp32c3/include/hal/i2s_ll.h), enforced by
// `i2s_channel_init_tdm_mode()` itself
// (components/esp_driver_i2s/i2s_tdm.c: "total slots(...) *
// slot_bit_width(...) exceeds the maximum 128" -> ESP_ERR_INVALID_ARG).
// The configuration below requests 16 slots x 16 bits = 256 bits/frame -
// the literal target geometry - which is 2x that hardware ceiling. This
// call is therefore EXPECTED to fail with ESP_ERR_INVALID_ARG, checked
// and logged like every other step; this is a prediction from the
// driver's own source code, not yet confirmed by running on real
// hardware in this session - see docs/si3050-clock-probe.md.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_tdm.h"

#include "si3050_clock_probe_generator_config.h"

using interbridge::configuredBclkHz;
using interbridge::configuredTdmRatio;

namespace {

// GPIO0 = PCLK, GPIO1 = FSYNC - the same pins the legacy probe and
// si3050_pins.h assign, preserved unchanged. Not #include-ing the
// Arduino-namespaced interbridge si3050_pins.h here: this is a plain
// ESP-IDF component with no Arduino dependency, and gpio_num_t (an
// ESP-IDF enum) needs an explicit cast from a plain integer anyway - the
// numeric values are identical and documented here instead.
constexpr gpio_num_t kPclkPin = GPIO_NUM_0;
constexpr gpio_num_t kFsyncPin = GPIO_NUM_1;

constexpr uint32_t kSampleRateHz = 8000;         // Si3050Config::fsyncHz
constexpr uint32_t kBitsPerSample = 16;
constexpr uint32_t kTotalSlots = 16;
constexpr i2s_tdm_slot_mask_t kAllSixteenSlots = static_cast<i2s_tdm_slot_mask_t>(
    I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3 | I2S_TDM_SLOT4 | I2S_TDM_SLOT5 |
    I2S_TDM_SLOT6 | I2S_TDM_SLOT7 | I2S_TDM_SLOT8 | I2S_TDM_SLOT9 | I2S_TDM_SLOT10 | I2S_TDM_SLOT11 |
    I2S_TDM_SLOT12 | I2S_TDM_SLOT13 | I2S_TDM_SLOT14 | I2S_TDM_SLOT15);

} // namespace

extern "C" void app_main(void) {
    i2s_chan_handle_t txHandle = nullptr;
    i2s_chan_config_t chanConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const esp_err_t newChannelResult = i2s_new_channel(&chanConfig, &txHandle, nullptr);

    // I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG (driver/i2s_tdm.h) sets
    // ws_width=1, ws_pol=true - a single-BCLK-wide, active-high frame
    // sync pulse, matching the Si3050 datasheet's FSYNC ("PCM Short
    // standard... period of synchronization signal (WS) is 1 bck
    // cycle") the same way the legacy probe's
    // I2S_COMM_FORMAT_STAND_PCM_SHORT did. Only the TDM driver header
    // documents this macro; nothing here is a guessed field.
    i2s_tdm_config_t tdmConfig = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kSampleRateHz),
        .slot_cfg = I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
                                                          kAllSixteenSlots),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED, // Si3050 does not use MCLK
                .bclk = kPclkPin,
                .ws = kFsyncPin,
                .dout = I2S_GPIO_UNUSED, // data content is irrelevant to this probe
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {},
            },
    };
    tdmConfig.slot_cfg.total_slot = kTotalSlots; // explicit 16, not I2S_TDM_AUTO_SLOT_NUM - "all 16 slots active"

    esp_err_t initResult = ESP_FAIL;
    if (newChannelResult == ESP_OK) {
        initResult = i2s_channel_init_tdm_mode(txHandle, &tdmConfig);
    }

    esp_err_t enableResult = ESP_FAIL;
    if (initResult == ESP_OK) {
        enableResult = i2s_channel_enable(txHandle);
    }

    const bool started = (newChannelResult == ESP_OK) && (initResult == ESP_OK) && (enableResult == ESP_OK);

    const uint32_t requestedRatio = configuredTdmRatio(kTotalSlots, kBitsPerSample);
    const uint32_t requestedPclkHz = configuredBclkHz(kSampleRateHz, kTotalSlots, kBitsPerSample);
    printf("[SI3050 CLOCK PROBE IDF5] requested_sample_rate_hz=%lu requested_total_slots=%lu "
          "requested_bits_per_sample=%lu requested_ratio=%lu requested_pclk_hz=%lu started=%s\n",
          static_cast<unsigned long>(kSampleRateHz), static_cast<unsigned long>(kTotalSlots),
          static_cast<unsigned long>(kBitsPerSample), static_cast<unsigned long>(requestedRatio),
          static_cast<unsigned long>(requestedPclkHz), started ? "true" : "false");
    printf("[SI3050 CLOCK PROBE IDF5] note: the line above reports what was requested, not a measurement - only "
          "esp32dev-si3050-clock-meter's real hardware measurement confirms actual frequencies\n");
    if (!started) {
        printf("[SI3050 CLOCK PROBE IDF5] i2s_new_channel=%d i2s_channel_init_tdm_mode=%d i2s_channel_enable=%d\n",
              static_cast<int>(newChannelResult), static_cast<int>(initResult), static_cast<int>(enableResult));
    }

    // Nothing else to do here: if started, the I2S peripheral generates
    // PCLK/FSYNC entirely in hardware from this point on. No sleeps,
    // busy-waits, LEDC, RMT, bit-banging, or delay-based approximation
    // is used for the clock itself - this idle loop only keeps the task
    // alive.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
