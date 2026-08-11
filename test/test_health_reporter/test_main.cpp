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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_first_call_is_always_due);
    RUN_TEST(test_not_due_before_interval_elapses);
    RUN_TEST(test_due_once_interval_elapses);
    RUN_TEST(test_force_next_publish_overrides_cadence);
    return UNITY_END();
}
