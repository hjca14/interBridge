#include "dev_ring_button.h"

namespace interbridge {

namespace {
// Wrap-safe deadline comparison, same technique as
// DevMqttSmokeState::deadlineReached() (mqtt_smoke_state.cpp) and
// ButtonController's debounce math (hardware/button.cpp).
bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}
} // namespace

DevRingButtonController::DevRingButtonController(IDevRingButtonInput& input, uint32_t debounceMs,
                                                  uint32_t lockoutMs)
    : input_(input),
      debounceMs_(debounceMs),
      lockoutMs_(lockoutMs),
      rawPressedLast_(false),
      lastRawChangeMs_(0),
      hasLastRawChange_(false),
      debouncedPressed_(false),
      lockoutActive_(false),
      lockoutUntilMs_(0) {}

bool DevRingButtonController::update(uint32_t nowMs) {
    bool raw = input_.isPressed();

    if (!hasLastRawChange_ || raw != rawPressedLast_) {
        rawPressedLast_ = raw;
        lastRawChangeMs_ = nowMs;
        hasLastRawChange_ = true;
    }

    bool stable = static_cast<uint32_t>(nowMs - lastRawChangeMs_) >= debounceMs_;
    bool newDebounced = stable ? raw : debouncedPressed_;

    if (lockoutActive_ && deadlineReached(nowMs, lockoutUntilMs_)) {
        lockoutActive_ = false;
    }

    bool risingEdge = newDebounced && !debouncedPressed_;
    debouncedPressed_ = newDebounced;

    if (!risingEdge) {
        return false;
    }
    if (lockoutActive_) {
        // A qualifying edge arrived while still locked out from a very
        // recent event (contact bounce resolving outside the debounce
        // window itself) - suppress it, but do not extend the lockout;
        // the button must still be released and pressed again cleanly.
        return false;
    }

    lockoutActive_ = true;
    lockoutUntilMs_ = nowMs + lockoutMs_;
    return true;
}

} // namespace interbridge
