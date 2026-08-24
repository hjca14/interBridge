#include "si3050_controller.h"

#include "si3050_timing.h"

namespace interbridge {

Si3050Controller::Si3050Controller(ISi3050Bus& bus, IPcmClock& clock, ISi3050Reset& reset, IDelayProvider& delay,
                                   const Si3050Config& config)
    : bus_(bus), clock_(clock), reset_(reset), delay_(delay), config_(config) {}

Si3050InitResult Si3050Controller::initialize() {
    if (ready_) return Si3050InitResult::Ready; // idempotent - never re-runs the bring-up sequence.

    if (config_.pclkHz == 0 || config_.fsyncHz == 0) {
        // 0. Fail closed on an invalid config, before touching the bus or
        // clock at all and before any timing math (which would otherwise
        // divide by pclkHz). Actively (re)assert /RESET rather than
        // relying on a collaborator's own construction-time default.
        reset_.setAsserted(true);
        return Si3050InitResult::InvalidConfig;
    }

    bus_.begin();
    bus_.setChipSelect(false);                     // 1. CS deselected
    reset_.setAsserted(true);                       // 2. /RESET asserted
    bus_.holdClockIdleHigh();                       // 3. SCLK high before RESET rises - selects PCM/SPI mode
    clock_.start(config_.pclkHz, config_.fsyncHz);  // 4. PCLK/FSYNC started

    if (!clock_.isRunning()) {
        // The PCM clock did not actually start - fail closed. /RESET
        // stays asserted (set above); neither wait below is meaningful
        // without a real clock, and SPI must not be authorized. ready_
        // stays false, so a later initialize() call retries from scratch
        // rather than being treated as a no-op.
        return Si3050InitResult::ClockNotRunning;
    }

    delay_.delayMicroseconds(
        si3050CyclesToMicroseconds(kSi3050MinPclkCyclesBeforeResetRelease, config_.pclkHz)); // 5. >= 10 PCLK cycles (tmr)

    reset_.setAsserted(false);                      // 6. /RESET released

    delay_.delayMicroseconds(si3050PllSettleMicroseconds(config_.pclkHz)); // 7. PLL settle (Tsettle = 64/FPCLK)

    ready_ = true;                                  // 8. SPI/future DAA configuration now permitted
    return Si3050InitResult::Ready;
}

bool Si3050Controller::isReady() const {
    return ready_;
}

std::optional<uint8_t> Si3050Controller::transferRaw(uint8_t out) {
    if (!ready_) return std::nullopt;
    return bus_.transfer(out);
}

} // namespace interbridge
