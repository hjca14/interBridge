#include <unity.h>

#include "../../src/hardware/gpio.h"
#include "../../src/intercom/line_detector.h"

using namespace interbridge;

namespace {

// Test double for IHardwareIO. Demonstrates that intercom logic is
// testable without any real hardware, despite the electrical interface
// still being undefined (see CONTEXT.md).
class MockHardware : public IHardwareIO {
public:
    bool line = false;
    bool doorEnabled = false;

    bool readLineState() override { return line; }
    void setDoorOutput(bool enabled) override { doorEnabled = enabled; }
};

} // namespace

void setUp() {}
void tearDown() {}

void test_first_update_establishes_baseline_without_event() {
    MockHardware hw;
    LineDetector detector(hw);
    auto event = detector.update();
    TEST_ASSERT_FALSE(event.has_value());
}

void test_line_going_active_emits_off_hook() {
    MockHardware hw;
    LineDetector detector(hw);
    detector.update(); // baseline: line == false

    hw.line = true;
    auto event = detector.update();

    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(EventType::OffHook), static_cast<int>(event->type));
}

void test_line_going_inactive_emits_on_hook() {
    MockHardware hw;
    hw.line = true;
    LineDetector detector(hw);
    detector.update(); // baseline: line == true

    hw.line = false;
    auto event = detector.update();

    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(EventType::OnHook), static_cast<int>(event->type));
}

void test_no_change_emits_no_event() {
    MockHardware hw;
    LineDetector detector(hw);
    detector.update();

    auto event = detector.update();

    TEST_ASSERT_FALSE(event.has_value());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_first_update_establishes_baseline_without_event);
    RUN_TEST(test_line_going_active_emits_off_hook);
    RUN_TEST(test_line_going_inactive_emits_on_hook);
    RUN_TEST(test_no_change_emits_no_event);
    return UNITY_END();
}
