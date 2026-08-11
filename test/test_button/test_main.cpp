#include <unity.h>

#include "../../src/hardware/button.h"

using namespace interbridge;

namespace {
class FakeButtonInput : public IButtonInput {
public:
    bool pressed = false;
    bool isPressed() override { return pressed; }
};
} // namespace

void setUp() {}
void tearDown() {}

void test_short_press_produces_no_action() {
    FakeButtonInput input;
    ButtonController controller(input);

    input.pressed = true;
    TEST_ASSERT_TRUE(controller.update(0) == ButtonAction::None);
    TEST_ASSERT_TRUE(controller.update(kButtonDebounceMs + 10) == ButtonAction::None);

    input.pressed = false;
    TEST_ASSERT_TRUE(controller.update(kButtonDebounceMs + 500) == ButtonAction::None);
}

void test_bounce_does_not_generate_action() {
    FakeButtonInput input;
    ButtonController controller(input);

    // Rapid flicker within the debounce window must never stabilize.
    for (uint32_t t = 0; t < kButtonDebounceMs * 3; t += 5) {
        input.pressed = (t / 5) % 2 == 0;
        TEST_ASSERT_TRUE(controller.update(t) == ButtonAction::None);
    }
}

void test_provisioning_threshold_fires_once() {
    FakeButtonInput input;
    ButtonController controller(input);

    input.pressed = true;
    controller.update(0); // establishes debounce baseline

    ButtonAction action = ButtonAction::None;
    for (uint32_t t = kButtonDebounceMs; t <= kButtonProvisioningHoldMs + 100; t += 50) {
        ButtonAction a = controller.update(t);
        if (a != ButtonAction::None) {
            TEST_ASSERT_TRUE(action == ButtonAction::None); // fires only once
            action = a;
        }
    }

    TEST_ASSERT_TRUE(action == ButtonAction::ProvisioningRequested);
}

void test_factory_reset_threshold_fires_once() {
    FakeButtonInput input;
    ButtonController controller(input);

    input.pressed = true;
    controller.update(0);

    ButtonAction provisioningAction = ButtonAction::None;
    ButtonAction resetAction = ButtonAction::None;
    for (uint32_t t = kButtonDebounceMs; t <= kButtonFactoryResetHoldMs + 100; t += 50) {
        ButtonAction a = controller.update(t);
        if (a == ButtonAction::ProvisioningRequested) {
            provisioningAction = a;
        } else if (a == ButtonAction::FactoryResetRequested) {
            TEST_ASSERT_TRUE(resetAction == ButtonAction::None); // fires only once
            resetAction = a;
        }
    }

    TEST_ASSERT_TRUE(provisioningAction == ButtonAction::ProvisioningRequested);
    TEST_ASSERT_TRUE(resetAction == ButtonAction::FactoryResetRequested);
}

void test_holding_past_reset_threshold_does_not_repeat_trigger() {
    FakeButtonInput input;
    ButtonController controller(input);

    input.pressed = true;
    controller.update(0);

    int resetFires = 0;
    for (uint32_t t = kButtonDebounceMs; t <= kButtonFactoryResetHoldMs + 5000; t += 200) {
        if (controller.update(t) == ButtonAction::FactoryResetRequested) {
            resetFires++;
        }
    }

    TEST_ASSERT_EQUAL(1, resetFires);
}

void test_new_press_after_release_can_fire_again() {
    FakeButtonInput input;
    ButtonController controller(input);

    input.pressed = true;
    controller.update(0);
    ButtonAction first = ButtonAction::None;
    for (uint32_t t = kButtonDebounceMs; t <= kButtonProvisioningHoldMs + 100; t += 50) {
        ButtonAction a = controller.update(t);
        if (a != ButtonAction::None) first = a;
    }
    TEST_ASSERT_TRUE(first == ButtonAction::ProvisioningRequested);

    // Release, wait past debounce, press again.
    uint32_t releaseTime = kButtonProvisioningHoldMs + 200;
    input.pressed = false;
    controller.update(releaseTime);
    controller.update(releaseTime + kButtonDebounceMs + 10);

    uint32_t secondPressStart = releaseTime + kButtonDebounceMs + 20;
    input.pressed = true;
    controller.update(secondPressStart);

    ButtonAction second = ButtonAction::None;
    for (uint32_t t = secondPressStart + kButtonDebounceMs; t <= secondPressStart + kButtonProvisioningHoldMs + 100;
         t += 50) {
        ButtonAction a = controller.update(t);
        if (a != ButtonAction::None) second = a;
    }
    TEST_ASSERT_TRUE(second == ButtonAction::ProvisioningRequested);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_short_press_produces_no_action);
    RUN_TEST(test_bounce_does_not_generate_action);
    RUN_TEST(test_provisioning_threshold_fires_once);
    RUN_TEST(test_factory_reset_threshold_fires_once);
    RUN_TEST(test_holding_past_reset_threshold_does_not_repeat_trigger);
    RUN_TEST(test_new_press_after_release_can_fire_again);
    return UNITY_END();
}
