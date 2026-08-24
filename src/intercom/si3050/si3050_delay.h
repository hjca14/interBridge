#pragma once

#include <cstdint>
#include <vector>

#include "si3050_call_log.h"

namespace interbridge {

// Short, blocking hardware-settling wait, used ONLY during the Si3050
// bring-up sequence (Si3050Controller::initialize()) for the two
// datasheet-mandated windows documented in si3050_timing.h (>=10 PCLK
// cycles before releasing RESET, and the PLL settle time after releasing
// it) - never in the main loop. Injectable so native tests never
// actually sleep.
class IDelayProvider {
public:
    virtual ~IDelayProvider() = default;
    virtual void delayMicroseconds(uint32_t microseconds) = 0;
};

// Real ESP32-C3 implementation, backed by Arduino delayMicroseconds().
// Blocking by design - only ever called from
// Si3050Controller::initialize(), a one-time bring-up step, never from
// the main loop. Not instantiated by the current firmware paths.
class Esp32Si3050Delay : public IDelayProvider {
public:
    void delayMicroseconds(uint32_t microseconds) override;
};

// Deterministic test double: records every call instead of actually
// waiting.
class FakeDelayProvider : public IDelayProvider {
public:
    explicit FakeDelayProvider(Si3050CallLog* log = nullptr) : log_(log) {}

    void delayMicroseconds(uint32_t microseconds) override;

    std::vector<uint32_t> calls;

private:
    Si3050CallLog* log_;
};

} // namespace interbridge
