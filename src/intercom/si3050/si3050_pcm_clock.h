#pragma once

#include <cstdint>

#include "si3050_call_log.h"

namespace interbridge {

// PCM clock generation (PCLK + FSYNC) for the Si3050. Target rates for
// Rev A: PCLK = 2.048 MHz, FSYNC = 8 kHz (see si3050_config.h). The Si3050
// datasheet's Section 5.30 "Clock Generation" requires PCLK to run at one
// of a fixed set of rates synchronous to an 8 kHz FSYNC.
class IPcmClock {
public:
    virtual ~IPcmClock() = default;

    virtual void start(uint32_t pclkHz, uint32_t fsyncHz) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

// Real ESP32-C3 implementation for GPIO0 (PCLK) / GPIO1 (FSYNC) - see
// si3050_pins.h. NOT implemented yet: generating a stable, phase-locked
// 2.048 MHz PCLK with a synchronous 8 kHz FSYNC frame pulse (including
// the pulse width/edge alignment required by datasheet Section 5.32
// "PCM Highway") needs a hardware timer/I2S peripheral configuration
// that is not verified in this repository - implementing it without that
// verification would risk guessing at exactly the kind of timing detail
// this foundation is required not to invent. isRunning() always reports
// false so callers never mistake this for a working clock. Not
// instantiated by the current firmware paths.
class Esp32PcmClock : public IPcmClock {
public:
    void start(uint32_t pclkHz, uint32_t fsyncHz) override;
    void stop() override;
    bool isRunning() const override;
};

// Deterministic test double.
class FakePcmClock : public IPcmClock {
public:
    explicit FakePcmClock(Si3050CallLog* log = nullptr) : log_(log) {}

    void start(uint32_t pclkHz, uint32_t fsyncHz) override;
    void stop() override;
    bool isRunning() const override;

    uint32_t lastPclkHz = 0;
    uint32_t lastFsyncHz = 0;
    // Lets a test simulate a clock that was asked to start but never
    // actually came up (e.g. real PCLK/FSYNC generation not implemented,
    // matching Esp32PcmClock) - start() sets running_ from this instead
    // of unconditionally true. Defaults to true so every other test's
    // FakePcmClock behaves like a working clock without opting in.
    bool startSucceeds = true;

private:
    void log(const char* tag);
    bool running_ = false;
    Si3050CallLog* log_;
};

} // namespace interbridge
