#include <unity.h>

#include "../../src/network/reconnect_manager.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_first_delay_never_exceeds_initial_delay() {
    FakeRandomSource random(42);
    ReconnectManager manager(random, 1000, 300000);

    uint32_t delay = manager.nextDelayMs();

    TEST_ASSERT_TRUE(delay <= 1000);
}

void test_delay_cap_grows_exponentially_then_clamps_to_max() {
    FakeRandomSource random(7);
    ReconnectManager manager(random, 1000, 10000);

    // attempt 0: cap=1000, attempt 1: cap=2000, attempt 2: cap=4000,
    // attempt 3: cap=8000, attempt 4: cap=min(16000,10000)=10000 (clamped)
    for (int i = 0; i < 4; i++) {
        uint32_t cap = 1000u << i;
        uint32_t delay = manager.nextDelayMs();
        TEST_ASSERT_TRUE(delay <= cap);
    }

    for (int i = 0; i < 10; i++) {
        uint32_t delay = manager.nextDelayMs();
        TEST_ASSERT_TRUE(delay <= 10000);
    }
}

void test_reset_restarts_backoff_from_initial_delay() {
    FakeRandomSource random(3);
    ReconnectManager manager(random, 1000, 300000);

    for (int i = 0; i < 10; i++) {
        manager.nextDelayMs();
    }
    TEST_ASSERT_TRUE(manager.attempt() > 0);

    manager.reset();
    TEST_ASSERT_EQUAL(0, manager.attempt());

    uint32_t delay = manager.nextDelayMs();
    TEST_ASSERT_TRUE(delay <= 1000);
}

void test_attempt_counter_increments() {
    FakeRandomSource random(1);
    ReconnectManager manager(random);

    TEST_ASSERT_EQUAL(0, manager.attempt());
    manager.nextDelayMs();
    TEST_ASSERT_EQUAL(1, manager.attempt());
    manager.nextDelayMs();
    TEST_ASSERT_EQUAL(2, manager.attempt());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_first_delay_never_exceeds_initial_delay);
    RUN_TEST(test_delay_cap_grows_exponentially_then_clamps_to_max);
    RUN_TEST(test_reset_restarts_backoff_from_initial_delay);
    RUN_TEST(test_attempt_counter_increments);
    return UNITY_END();
}
