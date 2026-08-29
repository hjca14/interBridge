#pragma once

#include <cstdint>

namespace interbridge {

// Phase 3B.8 bench-only DEV physical ring simulator. See
// docs/dev-ring-simulator.md. This is a separate, narrower state machine
// than hardware/button.h's ButtonController (which drives the physical
// config/reset button's hold-duration thresholds) - this one only ever
// cares about a single momentary press/release cycle.
constexpr uint32_t kDevRingButtonDebounceMs = 50;
// Extra lockout after a qualifying press is recognized, so contact bounce
// or electrical noise arriving shortly after the debounced edge (but
// outside the debounce window itself) cannot produce a second event
// before the button is genuinely released.
constexpr uint32_t kDevRingButtonLockoutMs = 250;

// Raw physical input. Must already report the debounced-free electrical
// reading inverted to "true means pressed" (i.e. the caller reads
// INPUT_PULLUP + active-low and negates it here) - see
// Esp32DevRingButtonInput in dev_ring_simulator_main.cpp.
class IDevRingButtonInput {
public:
    virtual ~IDevRingButtonInput() = default;
    virtual bool isPressed() = 0;
};

// Debounces a momentary button and reports a single "ring requested"
// pulse only on the debounced released-to-pressed transition: never
// while held, and never again until the button is released and pressed
// again. A short post-event lockout additionally guards against
// contact-bounce bursts that resolve just outside the debounce window.
class DevRingButtonController {
public:
    explicit DevRingButtonController(IDevRingButtonInput& input,
                                      uint32_t debounceMs = kDevRingButtonDebounceMs,
                                      uint32_t lockoutMs = kDevRingButtonLockoutMs);

    // Call every loop iteration with the current monotonic time. Returns
    // true at most once per physical press (see class comment).
    bool update(uint32_t nowMs);

private:
    IDevRingButtonInput& input_;
    uint32_t debounceMs_;
    uint32_t lockoutMs_;

    bool rawPressedLast_;
    uint32_t lastRawChangeMs_;
    bool hasLastRawChange_;

    bool debouncedPressed_;
    bool lockoutActive_;
    uint32_t lockoutUntilMs_;
};

} // namespace interbridge
