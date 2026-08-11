#include "button.h"

namespace interbridge {

bool Esp32ButtonInput::isPressed() {
    // TODO: not implemented - button GPIO and active level are not
    // defined yet. See CONTEXT.md > Open Questions.
    return false;
}

ButtonController::ButtonController(IButtonInput& input)
    : input_(input),
      rawPressedLast_(false),
      lastRawChangeMs_(0),
      hasLastRawChange_(false),
      debouncedPressed_(false),
      pressStartMs_(0),
      provisioningFired_(false),
      factoryResetFired_(false) {}

ButtonAction ButtonController::update(uint32_t nowMs) {
    bool raw = input_.isPressed();

    if (!hasLastRawChange_ || raw != rawPressedLast_) {
        rawPressedLast_ = raw;
        lastRawChangeMs_ = nowMs;
        hasLastRawChange_ = true;
    }

    bool stable = (nowMs - lastRawChangeMs_) >= kButtonDebounceMs;
    bool newDebounced = stable ? raw : debouncedPressed_;

    if (newDebounced && !debouncedPressed_) {
        // Debounced rising edge: a new press begins.
        pressStartMs_ = nowMs;
        provisioningFired_ = false;
        factoryResetFired_ = false;
    }

    debouncedPressed_ = newDebounced;

    if (!debouncedPressed_) {
        return ButtonAction::None;
    }

    uint32_t heldMs = nowMs - pressStartMs_;

    // Factory-reset threshold takes priority: if the hold jumps straight
    // past both thresholds between two update() calls, only the
    // strongest action fires.
    if (!factoryResetFired_ && heldMs >= kButtonFactoryResetHoldMs) {
        factoryResetFired_ = true;
        provisioningFired_ = true;
        return ButtonAction::FactoryResetRequested;
    }

    if (!provisioningFired_ && heldMs >= kButtonProvisioningHoldMs) {
        provisioningFired_ = true;
        return ButtonAction::ProvisioningRequested;
    }

    return ButtonAction::None;
}

} // namespace interbridge
