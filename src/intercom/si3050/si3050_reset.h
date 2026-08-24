#pragma once

#include "si3050_call_log.h"

namespace interbridge {

// /RESET line control. Active low per the Si3050 datasheet's pin
// description table ("An active low input that is used to reset all
// control registers...") and the Rev A hardware map (external pull-down
// keeps it asserted/low by default before firmware drives it).
class ISi3050Reset {
public:
    virtual ~ISi3050Reset() = default;

    // true = asserted (driven low), false = released (driven high).
    virtual void setAsserted(bool asserted) = 0;
    virtual bool isAsserted() const = 0;
};

// Real ESP32-C3 implementation for GPIO5 (see si3050_pins.h). A plain,
// pin-accurate GPIO operation - implemented for real. Not instantiated by
// the current esp32-c3/esp32-c3-dev-mqtt firmware paths - the board does
// not exist yet.
class Esp32Si3050Reset : public ISi3050Reset {
public:
    Esp32Si3050Reset();

    void setAsserted(bool asserted) override;
    bool isAsserted() const override;

private:
    bool asserted_;
};

// Deterministic test double.
class FakeSi3050Reset : public ISi3050Reset {
public:
    explicit FakeSi3050Reset(Si3050CallLog* log = nullptr) : log_(log) {}

    void setAsserted(bool asserted) override;
    bool isAsserted() const override;

private:
    void log(const char* tag);
    // Born asserted (true): mirrors the Rev A external pull-down, which
    // holds the real line low/asserted before firmware ever drives it -
    // not an arbitrary default.
    bool asserted_ = true;
    Si3050CallLog* log_;
};

} // namespace interbridge
