#include "clock.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

uint32_t Esp32Clock::monotonicMs() const {
#ifdef ARDUINO
    return millis();
#else
    return 0;
#endif
}

bool Esp32Clock::hasValidTime() const {
    // TODO: not implemented - NTP/time sync has not been wired up yet.
    // See CONTEXT.md > Open Questions.
    return false;
}

int64_t Esp32Clock::unixTimeSeconds() const {
    // TODO: not implemented - see hasValidTime().
    return 0;
}

FakeClock::FakeClock() : monotonicMs_(0), hasValidTime_(false), unixTimeSeconds_(0) {}

void FakeClock::setMonotonicMs(uint32_t ms) {
    monotonicMs_ = ms;
}

void FakeClock::advanceMs(uint32_t deltaMs) {
    monotonicMs_ += deltaMs;
}

void FakeClock::setUnixTimeSeconds(int64_t seconds) {
    unixTimeSeconds_ = seconds;
    hasValidTime_ = true;
}

void FakeClock::invalidateTime() {
    hasValidTime_ = false;
}

uint32_t FakeClock::monotonicMs() const {
    return monotonicMs_;
}

bool FakeClock::hasValidTime() const {
    return hasValidTime_;
}

int64_t FakeClock::unixTimeSeconds() const {
    return unixTimeSeconds_;
}

} // namespace interbridge
