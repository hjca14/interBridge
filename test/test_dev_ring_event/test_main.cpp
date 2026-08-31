#include <unity.h>

#include "../../src/dev/dev_ring_event.h"
#include "../../src/network/mqtt_transport.h"
#include "../../src/protocol/event_outbox.h"
#include "../../src/provisioning/device_identity.h"

using namespace interbridge;

namespace {
// Exactly "ib-" + 32 lowercase hex chars, matching the real, deployed
// isValidDeviceId() contract - asserted below (test_device_id_fixture_is_valid)
// rather than merely assumed, since this fixture value stands in for a
// real backend-facing device_id in every JSON-contract assertion.
constexpr const char* kTestDeviceId = "ib-0123456789abcdef0123456789abcdef";
// Exactly "evt-" + 32 lowercase hex chars, matching what generateHexId()
// actually produces (core/random_id.cpp) - used only as a fixed retry
// fixture, never generated through the coordinator.
constexpr const char* kTestEventId = "evt-0123456789abcdef0123456789abcdef";

class FakeDevRingButtonInput : public IDevRingButtonInput {
public:
    bool pressed = false;
    bool isPressed() override { return pressed; }
};

// event_id must be "evt-" followed by exactly 32 lowercase hex chars -
// the same contract core/random_id.cpp's generateHexId() implements.
bool matchesEventIdFormat(const std::string& id) {
    constexpr size_t kExpectedLength = 4 + 32; // "evt-" + 32 hex chars
    if (id.size() != kExpectedLength) return false;
    if (id.compare(0, 4, "evt-") != 0) return false;
    for (size_t i = 4; i < id.size(); i++) {
        char c = id[i];
        bool isLowercaseHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!isLowercaseHex) return false;
    }
    return true;
}

// Drives a press (or release) through DevRingEventCoordinator::update() -
// the same path the real firmware loop uses - so every press exercised by
// these tests genuinely goes through button debounce, event
// construction, AND outbox enqueue together. Calling the underlying
// DevRingButtonController directly instead (as an earlier version of this
// file did) would consume the debounced edge without ever reaching the
// coordinator, silently skipping the enqueue and leaving later
// outbox.pending() calls looking at an empty container.
std::string press(DevRingEventCoordinator& coordinator, FakeDevRingButtonInput& input, uint32_t& t,
                  bool hasValidTime = false, int64_t unixTimeSeconds = 0) {
    input.pressed = true;
    coordinator.update(t, hasValidTime, unixTimeSeconds); // records the raw change, not yet stable
    t += kDevRingButtonDebounceMs + 10;
    return coordinator.update(t, hasValidTime, unixTimeSeconds);
}

std::string release(DevRingEventCoordinator& coordinator, FakeDevRingButtonInput& input, uint32_t& t,
                    bool hasValidTime = false, int64_t unixTimeSeconds = 0) {
    input.pressed = false;
    coordinator.update(t, hasValidTime, unixTimeSeconds);
    t += kDevRingButtonDebounceMs + 10;
    return coordinator.update(t, hasValidTime, unixTimeSeconds);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_device_id_fixture_is_valid() {
    TEST_ASSERT_TRUE(isValidDeviceId(kTestDeviceId));
    TEST_ASSERT_TRUE(matchesEventIdFormat(kTestEventId));
}

void test_one_press_enqueues_exactly_one_ring_detected() {
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    MemoryEventOutbox outbox;
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, kTestDeviceId);

    uint32_t t = 0;
    std::string eventId = press(coordinator, input, t);

    TEST_ASSERT_FALSE(eventId.empty());
    TEST_ASSERT_TRUE(matchesEventIdFormat(eventId));
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));

    // Store the outbox snapshot in a real local variable before asserting
    // on it - outbox.pending() returns the vector by value, so binding a
    // reference/iterator straight into a temporary's element and using it
    // across later statements would dangle the moment that temporary is
    // destroyed at the end of its own full expression.
    auto pending = outbox.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pending.size()));
    const std::string json = pending[0].eventJson;
    TEST_ASSERT_TRUE(json.find("\"event\":\"RING_DETECTED\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(std::string("\"device_id\":\"") + kTestDeviceId + "\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"protocol_version\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(std::string("\"event_id\":\"") + eventId + "\"") != std::string::npos);
}

void test_holding_pressed_does_not_repeat() {
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    MemoryEventOutbox outbox;
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, kTestDeviceId);

    uint32_t t = 0;
    std::string firstId = press(coordinator, input, t);
    TEST_ASSERT_FALSE(firstId.empty());

    for (uint32_t held = t + 1; held < t + 2000; held += 50) {
        TEST_ASSERT_TRUE(coordinator.update(held, false, 0).empty());
    }
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));
}

void test_release_then_press_again_yields_a_new_event_id() {
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    MemoryEventOutbox outbox(8);
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, kTestDeviceId);

    uint32_t t = 0;
    std::string firstId = press(coordinator, input, t);
    TEST_ASSERT_FALSE(firstId.empty());

    t += kDevRingButtonLockoutMs + 100; // clear the post-event lockout
    std::string releaseResult = release(coordinator, input, t);
    TEST_ASSERT_TRUE(releaseResult.empty());
    std::string secondId = press(coordinator, input, t);

    TEST_ASSERT_FALSE(secondId.empty());
    TEST_ASSERT_TRUE(matchesEventIdFormat(secondId));
    TEST_ASSERT_TRUE(firstId != secondId);
    TEST_ASSERT_EQUAL(2, static_cast<int>(outbox.size()));
}

void test_timestamp_only_set_when_clock_is_valid() {
    FakeRandomSource random;

    FakeDevRingButtonInput inputValidClock;
    DevRingButtonController buttonValidClock(inputValidClock);
    MemoryEventOutbox outboxValidClock;
    DevRingEventCoordinator coordinatorValidClock(buttonValidClock, random, outboxValidClock, kTestDeviceId);
    uint32_t tValid = 0;
    std::string idValid = press(coordinatorValidClock, inputValidClock, tValid, /*hasValidTime=*/true, 1755000000);
    TEST_ASSERT_FALSE(idValid.empty());
    auto pendingValid = outboxValidClock.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pendingValid.size()));
    TEST_ASSERT_TRUE(pendingValid[0].eventJson.find("\"timestamp\":\"") != std::string::npos);

    FakeDevRingButtonInput inputNoClock;
    DevRingButtonController buttonNoClock(inputNoClock);
    MemoryEventOutbox outboxNoClock;
    DevRingEventCoordinator coordinatorNoClock(buttonNoClock, random, outboxNoClock, kTestDeviceId);
    uint32_t tInvalid = 0;
    std::string idInvalid = press(coordinatorNoClock, inputNoClock, tInvalid, /*hasValidTime=*/false, 1755000000);
    TEST_ASSERT_FALSE(idInvalid.empty());
    auto pendingInvalid = outboxNoClock.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pendingInvalid.size()));
    TEST_ASSERT_TRUE(pendingInvalid[0].eventJson.find("timestamp") == std::string::npos);
}

void test_retry_preserves_event_id_and_payload() {
    MemoryEventOutbox outbox;
    const std::string payload = std::string(R"({"protocol_version":1,"device_id":")") + kTestDeviceId +
                                R"(","event":"RING_DETECTED","event_id":")" + kTestEventId + R"("})";
    outbox.enqueue(kTestEventId, payload);
    FakeDeviceTransport transport;
    transport.connect(kTestDeviceId);
    transport.armPublishFailure(1); // first attempt fails, like a dropped publish

    size_t publishedFirst = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(0, static_cast<int>(publishedFirst));
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));

    auto pendingAfterFailure = outbox.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pendingAfterFailure.size()));
    TEST_ASSERT_EQUAL_STRING(kTestEventId, pendingAfterFailure[0].eventId.c_str());
    TEST_ASSERT_EQUAL_STRING(payload.c_str(), pendingAfterFailure[0].eventJson.c_str());

    size_t publishedSecond = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(1, static_cast<int>(publishedSecond));
    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));

    // Only the successful retry is ever recorded - armPublishFailure(1)
    // prevents the failed first attempt from being recorded at all (see
    // FakeDeviceTransport::publish()) - and it must carry the exact same
    // event_id/payload originally enqueued, never a regenerated one.
    const auto& published = transport.publishedMessages();
    TEST_ASSERT_EQUAL(1, static_cast<int>(published.size()));
    TEST_ASSERT_EQUAL_STRING(payload.c_str(), published[0].payload.c_str());
}

void test_offline_press_enqueues_and_reconnect_replays() {
    MemoryEventOutbox outbox;
    FakeDevRingButtonInput input;
    DevRingButtonController button(input);
    FakeRandomSource random;
    DevRingEventCoordinator coordinator(button, random, outbox, kTestDeviceId);

    uint32_t t = 0;
    std::string eventId = press(coordinator, input, t);
    TEST_ASSERT_FALSE(eventId.empty());
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size())); // enqueued with no transport involved at all

    FakeDeviceTransport transport; // never connected: simulates being offline
    TEST_ASSERT_FALSE(transport.isConnected());
    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size())); // still queued while offline

    transport.connect(kTestDeviceId); // reconnect
    size_t published = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(1, static_cast<int>(published));
    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));

    const auto& publishedMessages = transport.publishedMessages();
    TEST_ASSERT_EQUAL(1, static_cast<int>(publishedMessages.size()));
    TEST_ASSERT_TRUE(publishedMessages[0].payload.find(eventId) != std::string::npos);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_device_id_fixture_is_valid);
    RUN_TEST(test_one_press_enqueues_exactly_one_ring_detected);
    RUN_TEST(test_holding_pressed_does_not_repeat);
    RUN_TEST(test_release_then_press_again_yields_a_new_event_id);
    RUN_TEST(test_timestamp_only_set_when_clock_is_valid);
    RUN_TEST(test_retry_preserves_event_id_and_payload);
    RUN_TEST(test_offline_press_enqueues_and_reconnect_replays);
    return UNITY_END();
}
