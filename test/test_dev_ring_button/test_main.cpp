#include <unity.h>

#include "../../src/dev/dev_ring_button.h"

// The physical component wired to this DEV simulator is a "Linker
// Button" module (PCB with VCC/GND/SIG), not a bare dry-contact switch:
// its SIG line reads LOW when released and HIGH when pressed. This suite
// exercises DevRingButtonController - which is intentionally hardware-
// independent and knows nothing about voltage levels - through
// IDevRingButtonInput::isPressed(), so `pressed = true` here always means
// "the real adapter (Esp32DevRingButtonInput in
// src/dev/dev_ring_simulator_main.cpp) observed SIG=HIGH" and
// `pressed = false` means "SIG=LOW." The polarity mapping itself
// (digitalRead(pin) == HIGH) lives only in that Arduino-only adapter and
// is not natively testable; what IS tested here, at the abstraction
// boundary, is that the controller's logic produces the correct
// press-driven events regardless of which raw voltage level "pressed"
// happens to correspond to.
using namespace interbridge;

namespace {
class FakeDevRingButtonInput : public IDevRingButtonInput {
public:
    bool pressed = false; // true == SIG=HIGH (Linker Button pressed); false == SIG=LOW (released)
    bool isPressed() override { return pressed; }
};

// Drives a press (or release) to a fully debounced, settled state and
// returns update()'s result at the moment debounce completes. `t` is
// advanced past the settle point so the caller's next call starts clean.
// Every raw change needs two update() calls: one at the instant the
// change happens (recorded as the new debounce baseline, never itself
// stable) and one strictly after debounceMs has elapsed since then.
bool settle(DevRingButtonController& controller, FakeDevRingButtonInput& input, uint32_t& t, bool pressed) {
    input.pressed = pressed;
    controller.update(t); // records the raw change as the new baseline
    t += kDevRingButtonDebounceMs + 10;
    return controller.update(t);
}
} // namespace

void setUp() {}
void tearDown() {}

// Linker Button at rest (SIG=LOW / released) must never produce an event,
// no matter how many update() calls observe that resting state.
void test_released_low_state_produces_no_event() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);
    input.pressed = false; // SIG=LOW

    for (uint32_t t = 0; t < 2000; t += 25) {
        TEST_ASSERT_FALSE(controller.update(t));
    }
}

// The debounced LOW-to-HIGH transition (Linker Button released -> pressed)
// produces exactly one event.
void test_single_press_produces_exactly_one_event() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);
    uint32_t t = 0;

    TEST_ASSERT_TRUE(settle(controller, input, t, true));

    // Still held HIGH: no repeat.
    for (uint32_t held = t + 1; held < t + 2000; held += 50) {
        TEST_ASSERT_FALSE(controller.update(held));
    }
}

void test_bounce_within_debounce_window_does_not_duplicate() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);

    bool sawEvent = false;
    for (uint32_t t = 0; t < kDevRingButtonDebounceMs * 3; t += 5) {
        input.pressed = (t / 5) % 2 == 0; // rapid flicker, never stabilizes
        if (controller.update(t)) {
            TEST_ASSERT_FALSE(sawEvent); // fires at most once
            sawEvent = true;
        }
    }
    // The flicker never held stable for a full debounce window, so it
    // must never have stabilized into a press at all.
    TEST_ASSERT_FALSE(sawEvent);
}

// Held HIGH (Linker Button pressed and not released) never repeats.
void test_holding_pressed_never_repeats() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);

    input.pressed = true;
    int fireCount = 0;
    for (uint32_t t = 0; t < kDevRingButtonDebounceMs + 5000; t += 25) {
        if (controller.update(t)) fireCount++;
    }
    TEST_ASSERT_EQUAL(1, fireCount);
}

// HIGH -> LOW -> HIGH (pressed, released, pressed again) produces a new
// event on the second rising edge, once debounce/lockout have cleared.
void test_release_then_press_again_fires_second_event() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);
    uint32_t t = 0;

    TEST_ASSERT_TRUE(settle(controller, input, t, true));

    // Clear the post-event lockout before releasing/re-pressing.
    t += kDevRingButtonLockoutMs + 10;
    TEST_ASSERT_FALSE(settle(controller, input, t, false));
    TEST_ASSERT_TRUE(settle(controller, input, t, true));
}

void test_post_event_lockout_suppresses_burst_right_after_the_edge() {
    FakeDevRingButtonInput input;
    DevRingButtonController controller(input);
    uint32_t t = 0;

    TEST_ASSERT_TRUE(settle(controller, input, t, true));

    // A genuine release+re-press landing inside the lockout window (contact
    // bounce territory) must not produce a second event.
    TEST_ASSERT_TRUE(t < kDevRingButtonLockoutMs); // still inside lockout
    TEST_ASSERT_FALSE(settle(controller, input, t, false));
    TEST_ASSERT_FALSE(settle(controller, input, t, true));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_released_low_state_produces_no_event);
    RUN_TEST(test_single_press_produces_exactly_one_event);
    RUN_TEST(test_bounce_within_debounce_window_does_not_duplicate);
    RUN_TEST(test_holding_pressed_never_repeats);
    RUN_TEST(test_release_then_press_again_fires_second_event);
    RUN_TEST(test_post_event_lockout_suppresses_burst_right_after_the_edge);
    return UNITY_END();
}
