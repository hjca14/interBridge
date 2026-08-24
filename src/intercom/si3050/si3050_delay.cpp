#include "si3050_delay.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

void Esp32Si3050Delay::delayMicroseconds(uint32_t microseconds) {
#ifdef ARDUINO
    ::delayMicroseconds(microseconds);
#else
    (void)microseconds;
#endif
}

void FakeDelayProvider::delayMicroseconds(uint32_t microseconds) {
    calls.push_back(microseconds);
    if (log_) log_->emplace_back("delay.wait");
}

} // namespace interbridge
