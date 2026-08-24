#include "si3050_pcm_clock.h"

namespace interbridge {

void Esp32PcmClock::start(uint32_t pclkHz, uint32_t fsyncHz) {
    (void)pclkHz;
    (void)fsyncHz;
    // TODO: not implemented - see the class-level comment in
    // si3050_pcm_clock.h. No real PCLK/FSYNC waveform is generated.
}

void Esp32PcmClock::stop() {
    // TODO: not implemented - see start().
}

bool Esp32PcmClock::isRunning() const {
    return false;
}

void FakePcmClock::start(uint32_t pclkHz, uint32_t fsyncHz) {
    lastPclkHz = pclkHz;
    lastFsyncHz = fsyncHz;
    running_ = true;
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
