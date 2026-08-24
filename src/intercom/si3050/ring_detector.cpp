#include "ring_detector.h"

#include "si3050_pins.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

Esp32Si3050RingInput::Esp32Si3050RingInput() {
#ifdef ARDUINO
    pinMode(kSi3050PinRgdt, INPUT); // external 4.7k pull-up already present
#endif
}

bool Esp32Si3050RingInput::readRaw() {
#ifdef ARDUINO
    return digitalRead(kSi3050PinRgdt) == HIGH;
#else
    return true;
#endif
}

RingDetector::RingDetector(ISi3050RingInput& input, uint32_t debounceMs)
    : input_(input),
      debounceMs_(debounceMs),
      rawAssertedLast_(false),
      lastRawChangeMs_(0),
      hasLastRawChange_(false),
      debouncedAsserted_(false) {}

RingEvent RingDetector::update(uint32_t nowMs) {
    bool rawAsserted = !input_.readRaw(); // active low: electrically low = asserted

    if (!hasLastRawChange_ || rawAsserted != rawAssertedLast_) {
        rawAssertedLast_ = rawAsserted;
        lastRawChangeMs_ = nowMs;
        hasLastRawChange_ = true;
    }

    bool stable = (nowMs - lastRawChangeMs_) >= debounceMs_;
    bool newDebounced = stable ? rawAsserted : debouncedAsserted_;

    if (newDebounced == debouncedAsserted_) {
        return RingEvent::None;
    }

    debouncedAsserted_ = newDebounced;
    return newDebounced ? RingEvent::Asserted : RingEvent::Cleared;
}

bool RingDetector::isRingAsserted() const {
    return debouncedAsserted_;
}

} // namespace interbridge
