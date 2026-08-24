#include "si3050_reset.h"

#include "si3050_pins.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

Esp32Si3050Reset::Esp32Si3050Reset() : asserted_(true) {
#ifdef ARDUINO
    pinMode(kSi3050PinReset, OUTPUT);
    digitalWrite(kSi3050PinReset, LOW); // matches the external pull-down default: stay asserted until explicitly released.
#endif
}

void Esp32Si3050Reset::setAsserted(bool asserted) {
    asserted_ = asserted;
#ifdef ARDUINO
    digitalWrite(kSi3050PinReset, asserted ? LOW : HIGH); // active low
#endif
}

bool Esp32Si3050Reset::isAsserted() const {
    return asserted_;
}

void FakeSi3050Reset::setAsserted(bool asserted) {
    asserted_ = asserted;
    log(asserted ? "reset.assert" : "reset.release");
}

bool FakeSi3050Reset::isAsserted() const {
    return asserted_;
}

void FakeSi3050Reset::log(const char* tag) {
    if (log_) log_->emplace_back(tag);
}

} // namespace interbridge
