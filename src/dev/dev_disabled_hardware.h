#pragma once

#include "../hardware/gpio.h"
#include "../hardware/system_control.h"

namespace interbridge {

// Non-actuating stand-ins shared by every DEV bench entry point
// (esp32-c3-dev-mqtt, esp32-c3-dev-ring-simulator): CommandHandler needs a
// concrete IHardwareIO/ISystemControl to construct Intercom and to dispatch
// RESTART, but no DEV bench firmware may ever perform a real door/system
// action - see docs/mqtt-dev-smoke-test.md > Scope and safety. Kept in one
// place so the two DEV mains cannot silently diverge on this guarantee.
class DisabledHardware final : public IHardwareIO {
public:
    bool readLineState() override { return false; }
    bool setDoorOutput(bool) override { return false; }
};

class DisabledSystemControl final : public ISystemControl {
public:
    void restart() override {}
};

} // namespace interbridge
