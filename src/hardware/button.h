#pragma once

#include <cstdint>

namespace interbridge {

// Named thresholds for the physical config/reset button. See
// docs/communication-protocol.md > Physical Configuration / Reset
// Button. GPIO is intentionally not defined - see IButtonInput below.
constexpr uint32_t kButtonDebounceMs = 50;
constexpr uint32_t kButtonProvisioningHoldMs = 3000;
constexpr uint32_t kButtonFactoryResetHoldMs = 10000;

enum class ButtonAction {
    None,
    ProvisioningRequested,
    FactoryResetRequested,
};

// Raw physical input. The GPIO and electrical active level (pull-up vs.
// pull-down, active-high vs. active-low) are not defined yet - see
// CONTEXT.md > Open Questions. High-level code must depend only on this
// interface, never on a GPIO number.
class IButtonInput {
public:
    virtual ~IButtonInput() = default;
    virtual bool isPressed() = 0;
};

// Real ESP32 implementation. STUB: the button GPIO has not been assigned
// yet, so isPressed() always returns false. See CONTEXT.md.
class Esp32ButtonInput : public IButtonInput {
public:
    bool isPressed() override;
};

// Debounces IButtonInput and turns a hold duration into a one-shot
// semantic action. Each threshold fires at most once per continuous
// press (reset on release), so holding the button does not repeatedly
// trigger the same action. A short press (released before the
// provisioning threshold) never produces an action.
class ButtonController {
public:
    explicit ButtonController(IButtonInput& input);

    // Call frequently (e.g. every main loop iteration) with the current
    // monotonic time. Returns at most one action per call.
    ButtonAction update(uint32_t nowMs);

private:
    IButtonInput& input_;

    bool rawPressedLast_;
    uint32_t lastRawChangeMs_;
    bool hasLastRawChange_;

    bool debouncedPressed_;
    uint32_t pressStartMs_;
    bool provisioningFired_;
    bool factoryResetFired_;
};

} // namespace interbridge
