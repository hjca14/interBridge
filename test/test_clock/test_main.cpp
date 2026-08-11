#include <unity.h>

#include "../../src/hardware/clock.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_fake_clock_defaults_to_no_valid_time() {
    FakeClock clock;
    TEST_ASSERT_FALSE(clock.hasValidTime());
    TEST_ASSERT_EQUAL(0, static_cast<int>(clock.monotonicMs()));
}

void test_fake_clock_monotonic_advance() {
    FakeClock clock;
    clock.setMonotonicMs(100);
    clock.advanceMs(50);
    TEST_ASSERT_EQUAL(150, static_cast<int>(clock.monotonicMs()));
}

void test_fake_clock_wall_time_becomes_valid_when_set() {
    FakeClock clock;
    clock.setUnixTimeSeconds(1700000000);
    TEST_ASSERT_TRUE(clock.hasValidTime());
    TEST_ASSERT_EQUAL(1700000000, static_cast<int>(clock.unixTimeSeconds()));
}

void test_fake_clock_invalidate_time() {
    FakeClock clock;
    clock.setUnixTimeSeconds(1700000000);
    clock.invalidateTime();
    TEST_ASSERT_FALSE(clock.hasValidTime());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_fake_clock_defaults_to_no_valid_time);
    RUN_TEST(test_fake_clock_monotonic_advance);
    RUN_TEST(test_fake_clock_wall_time_becomes_valid_when_set);
    RUN_TEST(test_fake_clock_invalidate_time);
    return UNITY_END();
}
