#include <unity.h>

#include "../../src/core/events.h"
#include "../../src/core/state_machine.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_initial_state_is_boot() {
    StateMachine sm;
    TEST_ASSERT_EQUAL(static_cast<int>(State::Boot), static_cast<int>(sm.getState()));
}

void test_finish_boot_transitions_to_idle() {
    StateMachine sm;
    sm.finishBoot();
    TEST_ASSERT_EQUAL(static_cast<int>(State::Idle), static_cast<int>(sm.getState()));
}

void test_events_are_ignored_while_booting() {
    StateMachine sm;
    bool handled = sm.handleEvent(Event{EventType::RingDetected});
    TEST_ASSERT_FALSE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Boot), static_cast<int>(sm.getState()));
}

void test_idle_to_ringing_on_ring_detected() {
    StateMachine sm;
    sm.finishBoot();
    bool handled = sm.handleEvent(Event{EventType::RingDetected});
    TEST_ASSERT_TRUE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Ringing), static_cast<int>(sm.getState()));
}

void test_ringing_to_in_call_on_off_hook() {
    StateMachine sm;
    sm.finishBoot();
    sm.handleEvent(Event{EventType::RingDetected});
    bool handled = sm.handleEvent(Event{EventType::OffHook});
    TEST_ASSERT_TRUE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::InCall), static_cast<int>(sm.getState()));
}

void test_ringing_to_idle_on_on_hook_missed_call() {
    StateMachine sm;
    sm.finishBoot();
    sm.handleEvent(Event{EventType::RingDetected});
    bool handled = sm.handleEvent(Event{EventType::OnHook});
    TEST_ASSERT_TRUE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Idle), static_cast<int>(sm.getState()));
}

void test_in_call_to_idle_on_on_hook() {
    StateMachine sm;
    sm.finishBoot();
    sm.handleEvent(Event{EventType::RingDetected});
    sm.handleEvent(Event{EventType::OffHook});
    bool handled = sm.handleEvent(Event{EventType::OnHook});
    TEST_ASSERT_TRUE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Idle), static_cast<int>(sm.getState()));
}

void test_invalid_event_in_idle_is_ignored() {
    StateMachine sm;
    sm.finishBoot();
    // CallStarted is not a valid transition trigger from Idle.
    bool handled = sm.handleEvent(Event{EventType::CallStarted});
    TEST_ASSERT_FALSE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Idle), static_cast<int>(sm.getState()));
}

void test_report_fault_moves_to_error_from_any_state() {
    StateMachine sm;
    sm.finishBoot();
    sm.handleEvent(Event{EventType::RingDetected});
    sm.reportFault();
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(sm.getState()));
}

void test_error_state_ignores_events() {
    StateMachine sm;
    sm.finishBoot();
    sm.reportFault();
    bool handled = sm.handleEvent(Event{EventType::RingDetected});
    TEST_ASSERT_FALSE(handled);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(sm.getState()));
}

void test_transition_callback_is_invoked_with_correct_states() {
    static State lastFrom;
    static State lastTo;
    static int callCount;
    lastFrom = State::Boot;
    lastTo = State::Boot;
    callCount = 0;

    StateMachine sm;
    sm.setTransitionCallback([](State from, State to) {
        lastFrom = from;
        lastTo = to;
        callCount++;
    });
    sm.finishBoot();

    TEST_ASSERT_EQUAL(1, callCount);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Boot), static_cast<int>(lastFrom));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Idle), static_cast<int>(lastTo));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_initial_state_is_boot);
    RUN_TEST(test_finish_boot_transitions_to_idle);
    RUN_TEST(test_events_are_ignored_while_booting);
    RUN_TEST(test_idle_to_ringing_on_ring_detected);
    RUN_TEST(test_ringing_to_in_call_on_off_hook);
    RUN_TEST(test_ringing_to_idle_on_on_hook_missed_call);
    RUN_TEST(test_in_call_to_idle_on_on_hook);
    RUN_TEST(test_invalid_event_in_idle_is_ignored);
    RUN_TEST(test_report_fault_moves_to_error_from_any_state);
    RUN_TEST(test_error_state_ignores_events);
    RUN_TEST(test_transition_callback_is_invoked_with_correct_states);
    return UNITY_END();
}
