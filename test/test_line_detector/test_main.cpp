#include <unity.h>

#include "../../src/hardware/gpio.h"
#include "../../src/intercom/intercom.h"
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
    bool doorOutputSucceeds = true;

    bool readLineState() override { return line; }
    bool setDoorOutput(bool enabled) override {
        doorEnabled = enabled;
        return doorOutputSucceeds;
    }
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

void test_request_door_open_reports_hardware_success() {
    MockHardware hw;
    hw.doorOutputSucceeds = true;
    Intercom intercom(hw);

    bool opened = intercom.requestDoorOpen();

    TEST_ASSERT_TRUE(opened);
    TEST_ASSERT_TRUE(hw.doorEnabled);
}

void test_request_door_open_reports_hardware_failure() {
    // Esp32GpioHardware is currently a stub that cannot actuate anything
    // real; this proves Intercom does not fake success in that case.
    MockHardware hw;
    hw.doorOutputSucceeds = false;
    Intercom intercom(hw);

    bool opened = intercom.requestDoorOpen();

    TEST_ASSERT_FALSE(opened);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_first_update_establishes_baseline_without_event);
    RUN_TEST(test_line_going_active_emits_off_hook);
    RUN_TEST(test_line_going_inactive_emits_on_hook);
    RUN_TEST(test_no_change_emits_no_event);
    RUN_TEST(test_request_door_open_reports_hardware_success);
    RUN_TEST(test_request_door_open_reports_hardware_failure);
    return UNITY_END();
}
