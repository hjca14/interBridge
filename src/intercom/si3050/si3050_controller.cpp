#include "si3050_controller.h"

#include "si3050_timing.h"

namespace interbridge {

Si3050Controller::Si3050Controller(ISi3050Bus& bus, IPcmClock& clock, ISi3050Reset& reset, IDelayProvider& delay,
                                   const Si3050Config& config)
    : bus_(bus), clock_(clock), reset_(reset), delay_(delay), config_(config) {}

void Si3050Controller::initialize() {
    if (ready_) return; // idempotent - never re-runs the bring-up sequence.

    bus_.begin();
    bus_.setChipSelect(false);                     // 1. CS deselected
    reset_.setAsserted(true);                       // 2. /RESET asserted
    bus_.holdClockIdleHigh();                       // 3. SCLK high before RESET rises - selects PCM/SPI mode
    clock_.start(config_.pclkHz, config_.fsyncHz);  // 4. PCLK/FSYNC started

    delay_.delayMicroseconds(
        si3050CyclesToMicroseconds(kSi3050MinPclkCyclesBeforeResetRelease, config_.pclkHz)); // 5. >= 10 PCLK cycles (tmr)

    reset_.setAsserted(false);                      // 6. /RESET released

    delay_.delayMicroseconds(si3050PllSettleMicroseconds(config_.pclkHz)); // 7. PLL settle (Tsettle = 64/FPCLK)

    ready_ = true;                                  // 8. SPI/future DAA configuration now permitted
}

bool Si3050Controller::isReady() const {
    return ready_;
}

std::optional<uint8_t> Si3050Controller::transferRaw(uint8_t out) {
    if (!ready_) return std::nullopt;
    return bus_.transfer(out);
}

} // namespace interbridge
