#include <unity.h>

#include "../../src/network/health_reporter.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_first_call_is_always_due() {
    HealthReporter reporter(1000);
    TEST_ASSERT_TRUE(reporter.isDue(0));
}

void test_not_due_before_interval_elapses() {
    HealthReporter reporter(1000);
    reporter.isDue(0);
    TEST_ASSERT_FALSE(reporter.isDue(500));
    TEST_ASSERT_FALSE(reporter.isDue(999));
}

void test_due_once_interval_elapses() {
    HealthReporter reporter(1000);
    reporter.isDue(0);
    TEST_ASSERT_TRUE(reporter.isDue(1000));
}

void test_force_next_publish_overrides_cadence() {
    HealthReporter reporter(1000);
    reporter.isDue(0);
    TEST_ASSERT_FALSE(reporter.isDue(100));

    reporter.forceNextPublish();
    TEST_ASSERT_TRUE(reporter.isDue(150));

    // forced flag is consumed - cadence resumes from the forced time.
    TEST_ASSERT_FALSE(reporter.isDue(200));
    TEST_ASSERT_TRUE(reporter.isDue(150 + 1000));
}

void test_interval_is_wraparound_safe() {
    HealthReporter reporter(32);
    TEST_ASSERT_TRUE(reporter.isDue(0xfffffff0u));
    TEST_ASSERT_FALSE(reporter.isDue(0x0000000fu));
    TEST_ASSERT_TRUE(reporter.isDue(0x00000010u));
}

void test_failed_publish_policy_does_not_retry_in_a_tight_loop() {
    HealthReporter reporter(60000);
    // isDue records an attempt, regardless of transport result supplied by the
    // caller, so failure cannot produce a publish storm.
    TEST_ASSERT_TRUE(reporter.isDue(10));
    TEST_ASSERT_FALSE(reporter.isDue(11));
    TEST_ASSERT_FALSE(reporter.isDue(59010));
    TEST_ASSERT_TRUE(reporter.isDue(60010));
}

void test_dev_sixty_second_cadence_is_not_due_at_fifty_nine_seconds() {
    HealthReporter reporter(60000);
    TEST_ASSERT_TRUE(reporter.isDue(0));
    TEST_ASSERT_FALSE(reporter.isDue(59000));
    TEST_ASSERT_TRUE(reporter.isDue(60000));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_first_call_is_always_due);
    RUN_TEST(test_not_due_before_interval_elapses);
    RUN_TEST(test_due_once_interval_elapses);
    RUN_TEST(test_force_next_publish_overrides_cadence);
    RUN_TEST(test_interval_is_wraparound_safe);
    RUN_TEST(test_failed_publish_policy_does_not_retry_in_a_tight_loop);
    RUN_TEST(test_dev_sixty_second_cadence_is_not_due_at_fifty_nine_seconds);
    return UNITY_END();
}
