#include <unity.h>

#include "../../src/core/events.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_event_stores_its_type() {
    Event e{EventType::CallStarted};
    TEST_ASSERT_EQUAL(static_cast<int>(EventType::CallStarted), static_cast<int>(e.type));
}

void test_to_string_covers_all_known_events() {
    TEST_ASSERT_EQUAL_STRING("RING_DETECTED", toString(EventType::RingDetected));
    TEST_ASSERT_EQUAL_STRING("OFF_HOOK", toString(EventType::OffHook));
    TEST_ASSERT_EQUAL_STRING("ON_HOOK", toString(EventType::OnHook));
    TEST_ASSERT_EQUAL_STRING("DOOR_OPEN_REQUESTED", toString(EventType::DoorOpenRequested));
    TEST_ASSERT_EQUAL_STRING("CALL_STARTED", toString(EventType::CallStarted));
    TEST_ASSERT_EQUAL_STRING("CALL_ENDED", toString(EventType::CallEnded));
    TEST_ASSERT_EQUAL_STRING("WIFI_CONNECTED", toString(EventType::WifiConnected));
    TEST_ASSERT_EQUAL_STRING("WIFI_DISCONNECTED", toString(EventType::WifiDisconnected));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_event_stores_its_type);
    RUN_TEST(test_to_string_covers_all_known_events);
    return UNITY_END();
}
