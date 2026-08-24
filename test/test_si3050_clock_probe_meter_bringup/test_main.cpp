#include <unity.h>

#include "dev/si3050_clock_probe_meter_bringup.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_isr_service_ready_on_ok() {
    TEST_ASSERT_TRUE(isPcntIsrServiceReady(kClockProbeEspOk));
}

void test_isr_service_ready_when_already_installed() {
    // ESP-IDF documents ESP_ERR_INVALID_STATE specifically as "ISR
    // service already installed" for pcnt_isr_service_install() - not a
    // generic failure.
    TEST_ASSERT_TRUE(isPcntIsrServiceReady(kClockProbeEspErrInvalidState));
}

void test_isr_service_not_ready_on_generic_failure() {
    TEST_ASSERT_FALSE(isPcntIsrServiceReady(-1));   // ESP_FAIL
    TEST_ASSERT_FALSE(isPcntIsrServiceReady(0x101)); // ESP_ERR_NO_MEM - a real failure, not "already installed"
}

void test_tracker_starts_without_failure() {
    PcntBringupTracker tracker;
    TEST_ASSERT_FALSE(tracker.hasFailed());
    TEST_ASSERT_NULL(tracker.failedStepName());
    TEST_ASSERT_EQUAL(kClockProbeEspOk, tracker.failedStepResult());
}

void test_tracker_all_successful_steps_never_fail() {
    PcntBringupTracker tracker;
    tracker.record("pcnt_unit_config", kClockProbeEspOk, true);
    tracker.record("pcnt_counter_pause", kClockProbeEspOk, true);
    tracker.record("pcnt_counter_clear", kClockProbeEspOk, true);
    TEST_ASSERT_FALSE(tracker.hasFailed());
}

void test_tracker_records_first_failure_and_ignores_later_ones() {
    PcntBringupTracker tracker;
    tracker.record("pcnt_unit_config", kClockProbeEspOk, true);
    tracker.record("pcnt_isr_service_install", -1, false);
    TEST_ASSERT_TRUE(tracker.hasFailed());
    TEST_ASSERT_EQUAL_STRING("pcnt_isr_service_install", tracker.failedStepName());
    TEST_ASSERT_EQUAL(-1, tracker.failedStepResult());

    // A later, unrelated failure must not overwrite the root cause.
    tracker.record("pcnt_isr_handler_add", 0x101, false);
    TEST_ASSERT_EQUAL_STRING("pcnt_isr_service_install", tracker.failedStepName());
    TEST_ASSERT_EQUAL(-1, tracker.failedStepResult());

    // Nor can a later success clear a failure already recorded.
    tracker.record("pcnt_counter_resume", kClockProbeEspOk, true);
    TEST_ASSERT_TRUE(tracker.hasFailed());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_isr_service_ready_on_ok);
    RUN_TEST(test_isr_service_ready_when_already_installed);
    RUN_TEST(test_isr_service_not_ready_on_generic_failure);
    RUN_TEST(test_tracker_starts_without_failure);
    RUN_TEST(test_tracker_all_successful_steps_never_fail);
    RUN_TEST(test_tracker_records_first_failure_and_ignores_later_ones);
    return UNITY_END();
}
