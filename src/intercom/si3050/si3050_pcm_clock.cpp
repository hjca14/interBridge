#include "si3050_pcm_clock.h"

#include <cstdio>

#include "../../core/logger.h"
#include "si3050_pins.h"

#ifdef ARDUINO
#include <Arduino.h>
#include "driver/i2s.h"
#endif

namespace interbridge {

bool Si3050PcmClockBringup::shouldStart() const {
    return !running_;
}

void Si3050PcmClockBringup::recordDriverInstall(Si3050PcmClockEspErr result, bool succeeded) {
    if (hasFailed_) return;
    if (succeeded) {
        driverInstalled_ = true;
        return;
    }
    hasFailed_ = true;
    failedStepName_ = "i2s_driver_install";
    failedStepResult_ = result;
}

void Si3050PcmClockBringup::record(const char* stepName, Si3050PcmClockEspErr result, bool succeeded) {
    if (hasFailed_) return; // only the first (root-cause) failure is kept
    if (succeeded) return;
    hasFailed_ = true;
    failedStepName_ = stepName;
    failedStepResult_ = result;
}

void Si3050PcmClockBringup::markRunning() {
    if (hasFailed_) return;
    running_ = true;
}

bool Si3050PcmClockBringup::isRunning() const {
    return running_;
}

bool Si3050PcmClockBringup::hasFailed() const {
    return hasFailed_;
}

const char* Si3050PcmClockBringup::failedStepName() const {
    return failedStepName_;
}

Si3050PcmClockEspErr Si3050PcmClockBringup::failedStepResult() const {
    return failedStepResult_;
}

bool Si3050PcmClockBringup::shouldUninstall() const {
    return driverInstalled_;
}

void Si3050PcmClockBringup::recordUninstalled() {
    driverInstalled_ = false;
    running_ = false;
    hasFailed_ = false;
    failedStepName_ = nullptr;
    failedStepResult_ = kSi3050PcmClockEspOk;
}

#ifdef ARDUINO
namespace {

constexpr i2s_port_t kSi3050PcmI2sPort = I2S_NUM_0;

// Prints one sanitized line identifying exactly which bring-up step
// failed and its esp_err_t - no pin numbers or any other sensitive
// value, just the step name and a numeric code (mirrors the clock
// probe meter's reportBringupFailure()).
void logBringupFailure(const Si3050PcmClockBringup& bringup) {
    char message[80];
    std::snprintf(message, sizeof(message), "Esp32PcmClock bring-up failed step=%s esp_err=%ld",
                  bringup.failedStepName(), static_cast<long>(bringup.failedStepResult()));
    Logger::error(message);
}

} // namespace
#endif // ARDUINO

void Esp32PcmClock::start(uint32_t pclkHz, uint32_t fsyncHz) {
    if (!bringup_.shouldStart()) return; // idempotent: already running, never re-install

    if (!si3050PcmConfigurationSupported(pclkHz, fsyncHz)) {
        // Fail closed: this fixed 16 x 8 TDM geometry cannot honor a
        // different pclkHz for the given fsyncHz - never silently
        // substitute a different value than what was requested. Logged
        // on every platform (Logger is host-portable) - this is a pure
        // configuration check, not a real hardware call.
        char message[96];
        std::snprintf(message, sizeof(message),
                      "Esp32PcmClock: unsupported pclkHz=%lu for fsyncHz=%lu (16x8 TDM geometry requires pclkHz=%lu)",
                      static_cast<unsigned long>(pclkHz), static_cast<unsigned long>(fsyncHz),
                      static_cast<unsigned long>(
                          si3050PcmRequestedPclkHz(fsyncHz, kSi3050PcmTdmSlotCount, kSi3050PcmTdmSlotWidthBits)));
        Logger::error(message);
        return;
    }

#ifdef ARDUINO
    i2s_config_t i2sConfig = {};
    i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    i2sConfig.sample_rate = fsyncHz;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_8BIT; // kSi3050PcmTdmSlotWidthBits
    i2sConfig.channel_format = I2S_CHANNEL_FMT_MULTIPLE;  // TDM
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT; // 1-BCK-wide WS/FSYNC pulse
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 64; // 64 frames * 16 chan * 8 bit / 8 = 1024 bytes/buffer, under the driver's 4092-byte limit
    i2sConfig.use_apll = false;
    i2sConfig.tx_desc_auto_clear = true; // keep BCLK/WS toggling with silence if the (unused) TX data ever underflows
    i2sConfig.fixed_mclk = 0;
    // All 16 TDM slots active - only their *timing* matters here, data
    // content is left as zero-filled silence (see i2s_zero_dma_buffer()
    // below). Matches the physically validated clock probe geometry
    // exactly - see the class-level comment in si3050_pcm_clock.h.
    i2sConfig.chan_mask = static_cast<i2s_channel_t>(
        I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1 | I2S_TDM_ACTIVE_CH2 | I2S_TDM_ACTIVE_CH3 | I2S_TDM_ACTIVE_CH4 |
        I2S_TDM_ACTIVE_CH5 | I2S_TDM_ACTIVE_CH6 | I2S_TDM_ACTIVE_CH7 | I2S_TDM_ACTIVE_CH8 | I2S_TDM_ACTIVE_CH9 |
        I2S_TDM_ACTIVE_CH10 | I2S_TDM_ACTIVE_CH11 | I2S_TDM_ACTIVE_CH12 | I2S_TDM_ACTIVE_CH13 |
        I2S_TDM_ACTIVE_CH14 | I2S_TDM_ACTIVE_CH15);
    i2sConfig.total_chan = kSi3050PcmTdmSlotCount;
    i2sConfig.left_align = false;
    i2sConfig.big_edin = false;
    i2sConfig.bit_order_msb = false;
    i2sConfig.skip_msk = false;

    const esp_err_t installResult = i2s_driver_install(kSi3050PcmI2sPort, &i2sConfig, 0, nullptr);
    bringup_.recordDriverInstall(installResult, installResult == ESP_OK);
    if (bringup_.hasFailed()) {
        logBringupFailure(bringup_);
        return; // nothing was installed - no rollback needed
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
    pinConfig.bck_io_num = kSi3050PinPclk;  // GPIO0 - PCLK
    pinConfig.ws_io_num = kSi3050PinFsync;  // GPIO1 - FSYNC
    // DRX/DTX (PCM audio data) are deliberately NOT routed yet - this
    // class generates the clock signal only, matching the validated
    // probe. Wiring real audio data is future, separate work.
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

    const esp_err_t pinResult = i2s_set_pin(kSi3050PcmI2sPort, &pinConfig);
    bringup_.record("i2s_set_pin", pinResult, pinResult == ESP_OK);
    if (bringup_.hasFailed()) {
        logBringupFailure(bringup_);
        i2s_driver_uninstall(kSi3050PcmI2sPort); // roll back what this call acquired
        bringup_.recordUninstalled();
        return;
    }

    const esp_err_t zeroResult = i2s_zero_dma_buffer(kSi3050PcmI2sPort);
    bringup_.record("i2s_zero_dma_buffer", zeroResult, zeroResult == ESP_OK);
    if (bringup_.hasFailed()) {
        logBringupFailure(bringup_);
        i2s_driver_uninstall(kSi3050PcmI2sPort);
        bringup_.recordUninstalled();
        return;
    }

    bringup_.markRunning();
#else
    // Native build: no real I2S peripheral - isRunning() stays false,
    // matching every other real (Esp32*) Si3050 collaborator's
    // native-build behavior in this module. pclkHz/fsyncHz were already
    // consulted above (the config gate), so nothing is left unused.
#endif
}

void Esp32PcmClock::stop() {
#ifdef ARDUINO
    if (bringup_.shouldUninstall()) {
        i2s_driver_uninstall(kSi3050PcmI2sPort);
    }
#endif
    bringup_.recordUninstalled(); // safe even if nothing was installed/running
}

bool Esp32PcmClock::isRunning() const {
    return bringup_.isRunning();
}

void FakePcmClock::start(uint32_t pclkHz, uint32_t fsyncHz) {
    lastPclkHz = pclkHz;
    lastFsyncHz = fsyncHz;
    running_ = startSucceeds;
    log("clock.start");
}

void FakePcmClock::stop() {
    running_ = false;
    log("clock.stop");
}

bool FakePcmClock::isRunning() const {
    return running_;
}

void FakePcmClock::log(const char* tag) {
    if (log_) log_->emplace_back(tag);
}

} // namespace interbridge
