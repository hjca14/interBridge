#include <unity.h>

#include "intercom/si3050/ring_detector.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_first_call_establishes_baseline_without_an_event() {
    FakeSi3050RingInput input;
    RingDetector detector(input);

    input.electricallyHigh = true; // idle
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(0));
    TEST_ASSERT_FALSE(detector.isRingAsserted());
}

void test_low_level_asserts_ring_after_debounce() {
    FakeSi3050RingInput input;
    RingDetector detector(input, /*debounceMs=*/50);

    input.electricallyHigh = true;
    detector.update(0); // baseline

    input.electricallyHigh = false; // electrically low = asserted (active low)
    // The debounce window is measured from the call that first observes a
    // changed raw reading, not from the original baseline - t=10 here.
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(10));
    TEST_ASSERT_FALSE(detector.isRingAsserted());
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(59)); // 59-10=49ms: still inside the window
    TEST_ASSERT_TRUE(RingEvent::Asserted == detector.update(60)); // 60-10=50ms: stable
    TEST_ASSERT_TRUE(detector.isRingAsserted());
}

void test_high_level_clears_ring_after_debounce() {
    FakeSi3050RingInput input;
    RingDetector detector(input, /*debounceMs=*/50);

    input.electricallyHigh = false; // start asserted (raw low)
    detector.update(0); // baseline
    TEST_ASSERT_TRUE(RingEvent::Asserted == detector.update(50)); // 50-0=50ms: stable
    TEST_ASSERT_TRUE(detector.isRingAsserted());

    input.electricallyHigh = true; // goes idle again
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(60)); // change first observed here
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(109)); // 109-60=49ms: still inside the window
    TEST_ASSERT_TRUE(RingEvent::Cleared == detector.update(110)); // 110-60=50ms: stable
    TEST_ASSERT_FALSE(detector.isRingAsserted());
}

void test_debounce_ignores_short_noise_pulses() {
    FakeSi3050RingInput input;
    RingDetector detector(input, /*debounceMs=*/50);

    input.electricallyHigh = true;
    detector.update(0); // baseline

    // Rapid flicker within the debounce window must never stabilize into
    // an event, matching ButtonController's bounce-rejection behavior.
    for (uint32_t t = 5; t < 150; t += 5) {
        input.electricallyHigh = ((t / 5) % 2 == 0);
        RingEvent event = detector.update(t);
        TEST_ASSERT_TRUE(RingEvent::None == event);
    }
    TEST_ASSERT_FALSE(detector.isRingAsserted());
}

void test_debounce_interval_is_configurable() {
    FakeSi3050RingInput input;
    RingDetector detector(input, /*debounceMs=*/200);

    input.electricallyHigh = true;
    detector.update(0); // baseline

    input.electricallyHigh = false; // change first observed at t=10
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(10));
    TEST_ASSERT_TRUE(RingEvent::None == detector.update(209)); // 209-10=199ms: shorter than this detector's 200ms window
    TEST_ASSERT_TRUE(RingEvent::Asserted == detector.update(210)); // 210-10=200ms: stable
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_call_establishes_baseline_without_an_event);
    RUN_TEST(test_low_level_asserts_ring_after_debounce);
    RUN_TEST(test_high_level_clears_ring_after_debounce);
    RUN_TEST(test_debounce_ignores_short_noise_pulses);
    RUN_TEST(test_debounce_interval_is_configurable);
    return UNITY_END();
}
