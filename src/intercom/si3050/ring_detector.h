#pragma once

#include <cstdint>

namespace interbridge {

// Raw electrical read of /RGDT (GPIO8). Open-drain, active low, external
// 4.7k pull-up per the Rev A hardware map and the Si3050 datasheet
// ("RGDT... open-drain output... requires a 4.7k pullup... defaults to
// active low"). This interface has no dependency on IHardwareIO or any
// door-actuation path.
class ISi3050RingInput {
public:
    virtual ~ISi3050RingInput() = default;

    // true = electrically high (idle), false = electrically low (ring
    // condition signaled). See RingDetector for the debounced/sanitized
    // interpretation - this is the raw, unfiltered pin state.
    virtual bool readRaw() = 0;
};

// Real ESP32-C3 implementation for GPIO8 (see si3050_pins.h). External
// pull-up already present on Rev A, so no internal pull is configured.
// Not instantiated by the current esp32-c3/esp32-c3-dev-mqtt firmware
// paths - the board does not exist yet.
class Esp32Si3050RingInput : public ISi3050RingInput {
public:
    Esp32Si3050RingInput();

    bool readRaw() override;
};

// Deterministic test double.
class FakeSi3050RingInput : public ISi3050RingInput {
public:
    bool readRaw() override { return electricallyHigh; }

    // Idle by default, matching the external pull-up.
    bool electricallyHigh = true;
};

enum class RingEvent { None, Asserted, Cleared };

// Anti-noise debounce interval for /RGDT, deliberately NOT sourced from
// the datasheet: during an actual ring burst, RGDT itself toggles at the
// ring cadence (datasheet Section 5.18 "Ring Detection": the RGDT output
// frequency follows the ring signal frequency), so this class does not
// attempt to validate a real ring pattern - it only reports a debounced
// electrical level change, ignoring short glitches. Real ring pattern
// validation is explicitly out of scope - see docs/si3050-bringup.md.
constexpr uint32_t kRingDetectorDebounceMsDefault = 50;

// Debounces ISi3050RingInput and reports sanitized level-change events.
// Polling-based (no ISR) by design for this first step. Does not touch
// audio, intercom line state, or publish any MQTT event - that
// integration is explicitly future work. This class has no dependency on
// IHardwareIO or any door-actuation path by construction - it does not
// take one as a collaborator.
class RingDetector {
public:
    explicit RingDetector(ISi3050RingInput& input, uint32_t debounceMs = kRingDetectorDebounceMsDefault);

    // Call frequently (e.g. every main loop iteration) with the current
    // monotonic time. Returns at most one event per call. Returns None on
    // the first call (used to establish a debounce baseline, matching
    // LineDetector/ButtonController's convention elsewhere in this repo).
    RingEvent update(uint32_t nowMs);

    bool isRingAsserted() const;

private:
    ISi3050RingInput& input_;
    uint32_t debounceMs_;

    bool rawAssertedLast_;
    uint32_t lastRawChangeMs_;
    bool hasLastRawChange_;

    bool debouncedAsserted_;
};

} // namespace interbridge
