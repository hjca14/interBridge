#pragma once

#include <cstdint>

#include "si3050_call_log.h"

namespace interbridge {

// SPI bus + chip-select abstraction for the Si3050. No register format is
// implied by this interface - transfer() is a raw, full-duplex
// single-byte SPI exchange (Si3050 datasheet Table 7, "Switching
// Characteristics-Serial Peripheral Interface").
class ISi3050Bus {
public:
    virtual ~ISi3050Bus() = default;

    // Configures pin modes. Must not toggle CS, SCLK, or any data line by
    // itself - callers control those explicitly (see holdClockIdleHigh()
    // and setChipSelect()) because the Si3050 bring-up sequence requires
    // precise control before any real SPI transaction happens. Safe to
    // call more than once.
    virtual void begin() = 0;

    // Chip select. true = selected (driven low - CS is active low per the
    // datasheet's pin description table), false = deselected (high).
    virtual void setChipSelect(bool selected) = 0;

    // Drives SCLK to its idle-high level without performing a real SPI
    // transaction. Used only during bring-up, before RESET is released:
    // the Si3050 samples SCLK's level at that instant to select PCM/SPI
    // mode vs. GCI mode (datasheet Section 5.31 "Communication Interface
    // Mode Selection", Table 20).
    virtual void holdClockIdleHigh() = 0;

    // Full-duplex single-byte SPI transfer. Callers (Si3050Controller)
    // must never invoke this before bring-up completes.
    virtual uint8_t transfer(uint8_t out) = 0;
};

// Real ESP32-C3 implementation for the Rev A pin assignments (see
// si3050_pins.h). CS control and holding SCLK idle-high are plain,
// pin-accurate GPIO operations and are implemented for real; transfer()
// is intentionally NOT implemented yet - the SPI transaction electrical
// mode (clock polarity/phase) for the Si3050 is not confirmed against
// real hardware in this repository, so guessing it here would risk
// silently talking to the part incorrectly. Not instantiated by the
// current esp32-c3/esp32-c3-dev-mqtt firmware paths - the board does not
// exist yet.
class Esp32Si3050Bus : public ISi3050Bus {
public:
    void begin() override;
    void setChipSelect(bool selected) override;
    void holdClockIdleHigh() override;
    uint8_t transfer(uint8_t out) override;
};

// Deterministic test double. Records every call (optionally into a shared
// Si3050CallLog for cross-object ordering assertions) and lets tests
// script transfer() return values.
class FakeSi3050Bus : public ISi3050Bus {
public:
    explicit FakeSi3050Bus(Si3050CallLog* log = nullptr) : log_(log) {}

    void begin() override;
    void setChipSelect(bool selected) override;
    void holdClockIdleHigh() override;
    uint8_t transfer(uint8_t out) override;

    bool beginCalled = false;
    // false = deselected (CS high) - the fake's own construction-time
    // default, matching the required "CS is born deselected" contract
    // independent of anything Si3050Controller does.
    bool chipSelected = false;
    bool sclkHeldHigh = false;
    int transferCallCount = 0;
    uint8_t lastTransferOut = 0;
    uint8_t nextTransferReturn = 0;

private:
    void log(const char* tag);
    Si3050CallLog* log_;
};

} // namespace interbridge
