#include "system_control.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

void Esp32SystemControl::restart() {
#ifdef ARDUINO
    ESP.restart();
#endif
}

FakeSystemControl::FakeSystemControl() : restartCount_(0) {}

void FakeSystemControl::restart() {
    restartCount_++;
}

int FakeSystemControl::restartCount() const {
    return restartCount_;
}

} // namespace interbridge
