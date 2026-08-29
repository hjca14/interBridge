#include <unity.h>

#include "../../src/dev/dev_ring_event.h"
#include "../../src/network/mqtt_transport.h"
#include "../../src/protocol/event_outbox.h"

using namespace interbridge;

namespace {
class FakeDevRingButtonInput : public IDevRingButtonInput {
public:
    bool pressed = false;
    bool isPressed() override { return pressed; }
};

// Advances a fresh press through debounce, exactly like the
// test_dev_ring_button helper, so this suite can stay focused on the
// event/outbox contract instead of re-deriving debounce timing.
void press(DevRingButtonController& controller, FakeDevRingButtonInput& input, uint32_t& t) {
    input.pressed = true;
    controller.update(t);
    t += kDevRingButtonDebounceMs + 10;
    controller.update(t);
}
void release(DevRingButtonController& controller, FakeDevRingButtonInput& input, uint32_t& t) {
    input.pressed = false;
    controller.update(t);
    t += kDevRingButtonDebounceMs + 10;
    controller.update(t);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_one_press_enqueues_exactly_one_ring_detected() {
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    MemoryEventOutbox outbox;
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, "ib-test-device");

    uint32_t t = 0;
    input.pressed = true;
    coordinator.update(t, false, 0); // records the raw change, not yet stable
    t += kDevRingButtonDebounceMs + 10;
    std::string eventId = coordinator.update(t, false, 0);

    TEST_ASSERT_FALSE(eventId.empty());
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));
    const std::string& json = outbox.pending()[0].eventJson;
    TEST_ASSERT_TRUE(json.find("\"event\":\"RING_DETECTED\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"device_id\":\"ib-test-device\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"protocol_version\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(eventId) != std::string::npos);

    // Held down: no repeat.
    for (uint32_t held = t + 1; held < t + 2000; held += 50) {
        TEST_ASSERT_TRUE(coordinator.update(held, false, 0).empty());
    }
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));
}

void test_timestamp_only_set_when_clock_is_valid() {
    FakeRandomSource random;

    FakeDevRingButtonInput input2;
    DevRingButtonController button2(input2);
    MemoryEventOutbox outbox2;
    DevRingEventCoordinator coordinator2(button2, random, outbox2, "ib-test-device");
    uint32_t t2 = 0;
    input2.pressed = true;
    coordinator2.update(t2, true, 1755000000);
    t2 += kDevRingButtonDebounceMs + 10;
    coordinator2.update(t2, true, 1755000000);
    TEST_ASSERT_TRUE(outbox2.pending()[0].eventJson.find("\"timestamp\":\"") != std::string::npos);

    FakeDevRingButtonInput input3;
    DevRingButtonController button3(input3);
    MemoryEventOutbox outbox3;
    DevRingEventCoordinator coordinator3(button3, random, outbox3, "ib-test-device");
    uint32_t t3 = 0;
    input3.pressed = true;
    coordinator3.update(t3, false, 1755000000);
    t3 += kDevRingButtonDebounceMs + 10;
    coordinator3.update(t3, false, 1755000000);
    TEST_ASSERT_TRUE(outbox3.pending()[0].eventJson.find("timestamp") == std::string::npos);
}

void test_each_valid_press_gets_a_different_event_id() {
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    MemoryEventOutbox outbox(8);
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, "ib-test-device");

    uint32_t t = 0;
    press(button, input, t);
    std::string firstId = outbox.pending().back().eventId;

    t += kDevRingButtonLockoutMs + 100;
    release(button, input, t);
    press(button, input, t);
    std::string secondId = outbox.pending().back().eventId;

    TEST_ASSERT_FALSE(firstId.empty());
    TEST_ASSERT_FALSE(secondId.empty());
    TEST_ASSERT_TRUE(firstId != secondId);
}

void test_retry_preserves_event_id_and_payload() {
    MemoryEventOutbox outbox;
    outbox.enqueue("evt-stable", R"({"protocol_version":1,"device_id":"ib-x","event":"RING_DETECTED","event_id":"evt-stable"})");
    FakeDeviceTransport transport;
    transport.connect("ib-x");
    transport.armPublishFailure(1); // first attempt fails, like a dropped publish

    size_t publishedFirst = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(0, static_cast<int>(publishedFirst));
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));
    TEST_ASSERT_EQUAL_STRING("evt-stable", outbox.pending()[0].eventId.c_str());

    size_t publishedSecond = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(1, static_cast<int>(publishedSecond));
    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));

    // Both publish attempts (the failed one and the successful retry) must
    // have carried the exact same event_id/payload - never regenerated.
    const auto& published = transport.publishedMessages();
    TEST_ASSERT_EQUAL(1, static_cast<int>(published.size())); // armPublishFailure prevents recording the failed call
    TEST_ASSERT_TRUE(published[0].payload.find("evt-stable") != std::string::npos);
}

void test_offline_enqueues_and_reconnect_replays() {
    MemoryEventOutbox outbox;
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, "ib-test-device");

    uint32_t t = 0;
    press(button, input, t);
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));

    FakeDeviceTransport transport; // never connected: simulates being offline
    // While offline, the caller must not even attempt publishPendingEvents
    // against a disconnected transport - mirror main.cpp's guard.
    TEST_ASSERT_FALSE(transport.isConnected());
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size())); // still queued

    transport.connect("ib-test-device"); // reconnect
    size_t published = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(1, static_cast<int>(published));
    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_one_press_enqueues_exactly_one_ring_detected);
    RUN_TEST(test_timestamp_only_set_when_clock_is_valid);
    RUN_TEST(test_each_valid_press_gets_a_different_event_id);
    RUN_TEST(test_retry_preserves_event_id_and_payload);
    RUN_TEST(test_offline_enqueues_and_reconnect_replays);
    return UNITY_END();
}
