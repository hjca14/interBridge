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

class FakeDevRingButtonInput : public IDevRingButtonInput {
public:
    bool pressed = false;
    bool isPressed() override { return pressed; }
};

// "<prefix>-" followed by exactly 32 lowercase hex chars - the same
// contract core/random_id.cpp's generateHexId() implements for both
// event_id ("evt-") and call_id ("call-").
bool matchesPrefixedHexId(const std::string& id, const std::string& prefix) {
    const std::string expectedPrefix = prefix + "-";
    if (id.size() != expectedPrefix.size() + 32) return false;
    if (id.compare(0, expectedPrefix.size(), expectedPrefix) != 0) return false;
    for (size_t i = expectedPrefix.size(); i < id.size(); i++) {
        char c = id[i];
        bool isLowercaseHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!isLowercaseHex) return false;
    }
    return true;
}

bool matchesEventIdFormat(const std::string& id) { return matchesPrefixedHexId(id, "evt"); }
bool matchesCallIdFormat(const std::string& id) { return matchesPrefixedHexId(id, "call"); }

// Bundles the two-button composition dev_ring_simulator_main.cpp actually
// wires (GPIO4 "start" / GPIO3 "end" - see dev_ring_simulator_config.h),
// so every test here exercises the exact same object graph the real
// firmware loop drives, not a simplified stand-in.
struct Fixture {
    explicit Fixture(uint32_t timeoutMs = kDevCallTimeoutMs)
        : startButton(startInput),
          endButton(endInput),
          coordinator(startButton, endButton, random, outbox, kTestDeviceId, timeoutMs) {}

    FakeDevRingButtonInput startInput;
    FakeDevRingButtonInput endInput;
    DevRingButtonController startButton;
    DevRingButtonController endButton;
    MemoryEventOutbox outbox;
    FakeRandomSource random;
    DevRingEventCoordinator coordinator;
};

// Drives one full press (raw change, then the confirming call once the
// debounce window has elapsed) through DevRingEventCoordinator::update()
// - the same path the real firmware loop uses for both buttons, so every
// press exercised by these tests genuinely goes through button debounce,
// state-machine effect, AND outbox enqueue together. Calling
// DevRingButtonController::update() directly instead would consume the
// debounced edge without ever reaching the coordinator - see CONTEXT.md's
// record of the exact defect this pattern was introduced to avoid.
DevCallSessionOutcome pressStart(Fixture& f, uint32_t& t) {
    f.startInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    return f.coordinator.update(t, false, 0);
}

DevCallSessionOutcome releaseStart(Fixture& f, uint32_t& t) {
    f.startInput.pressed = false;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    return f.coordinator.update(t, false, 0);
}

DevCallSessionOutcome pressEnd(Fixture& f, uint32_t& t) {
    f.endInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    return f.coordinator.update(t, false, 0);
}

DevCallSessionOutcome releaseEnd(Fixture& f, uint32_t& t) {
    f.endInput.pressed = false;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    return f.coordinator.update(t, false, 0);
}

// Clears both buttons' post-event lockout so a following press is read as
// a genuinely new edge, mirroring test_release_then_press_again's pattern
// in the pre-existing single-button suite this file evolves from.
void clearLockout(uint32_t& t) { t += kDevRingButtonLockoutMs + 100; }
} // namespace

void setUp() {}
void tearDown() {}

void test_device_id_fixture_is_valid() { TEST_ASSERT_TRUE(isValidDeviceId(kTestDeviceId)); }

// 1. GPIO4 in Idle creates a session and a RING_DETECTED.
void test_gpio4_in_idle_creates_session_and_ring_detected() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome outcome = pressStart(f, t);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallState::Ringing), static_cast<int>(f.coordinator.state()));
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size()));
    TEST_ASSERT_EQUAL_STRING(outcome.callId.c_str(), f.coordinator.activeCallId().c_str());

    auto pending = f.outbox.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pending.size()));
    const std::string json = pending[0].eventJson;
    TEST_ASSERT_TRUE(json.find("\"event\":\"RING_DETECTED\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(std::string("\"device_id\":\"") + kTestDeviceId + "\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"protocol_version\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(std::string("\"call_id\":\"") + outcome.callId + "\"") != std::string::npos);
}

// 2. RING_DETECTED carries a distinctly-formatted event_id and call_id.
void test_ring_detected_event_id_and_call_id_are_distinct() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome outcome = pressStart(f, t);

    TEST_ASSERT_TRUE(matchesEventIdFormat(outcome.eventId));
    TEST_ASSERT_TRUE(matchesCallIdFormat(outcome.callId));
    TEST_ASSERT_TRUE(outcome.eventId != outcome.callId);
}

// 3. GPIO3 reuses the exact same call_id in RING_ENDED.
void test_gpio3_reuses_the_same_call_id_in_ring_ended() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome started = pressStart(f, t);
    clearLockout(t);
    DevCallSessionOutcome ended = pressEnd(f, t);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByButton), static_cast<int>(ended.kind));
    TEST_ASSERT_EQUAL_STRING(started.callId.c_str(), ended.callId.c_str());
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallState::Idle), static_cast<int>(f.coordinator.state()));
    TEST_ASSERT_TRUE(f.coordinator.activeCallId().empty());

    auto pending = f.outbox.pending();
    TEST_ASSERT_EQUAL(2, static_cast<int>(pending.size()));
    TEST_ASSERT_TRUE(pending[1].eventJson.find("\"event\":\"RING_ENDED\"") != std::string::npos);
    TEST_ASSERT_TRUE(pending[1].eventJson.find(std::string("\"call_id\":\"") + started.callId + "\"") !=
                     std::string::npos);
}

// 4. RING_DETECTED and RING_ENDED never share an event_id.
void test_ring_detected_and_ring_ended_have_different_event_ids() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome started = pressStart(f, t);
    clearLockout(t);
    DevCallSessionOutcome ended = pressEnd(f, t);

    TEST_ASSERT_TRUE(matchesEventIdFormat(started.eventId));
    TEST_ASSERT_TRUE(matchesEventIdFormat(ended.eventId));
    TEST_ASSERT_TRUE(started.eventId != ended.eventId);
}

// 5. GPIO3 in Idle (no active call) is ignored.
void test_gpio3_in_idle_is_ignored() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome outcome = pressEnd(f, t);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::EndIgnoredNoActiveCall),
                      static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(0, static_cast<int>(f.outbox.size()));
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallState::Idle), static_cast<int>(f.coordinator.state()));
}

// 6. GPIO4 while already Ringing is ignored: no new call_id, no new event.
void test_gpio4_while_ringing_is_ignored() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome first = pressStart(f, t);
    clearLockout(t);
    // A genuine second rising edge on GPIO4 (release, then press again)
    // while the session is still Ringing (GPIO3 never pulsed) - not just
    // holding the first press, which DevRingButtonController would
    // already suppress on its own before the coordinator ever saw it.
    releaseStart(f, t);
    clearLockout(t);
    DevCallSessionOutcome second = pressStart(f, t);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::StartIgnoredAlreadyRinging),
                      static_cast<int>(second.kind));
    TEST_ASSERT_EQUAL_STRING(first.callId.c_str(), f.coordinator.activeCallId().c_str());
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size()));
}

// 7. Contact bounce on either GPIO does not duplicate the session event.
void test_debounce_prevents_duplicate_on_both_gpios() {
    Fixture f;
    uint32_t t = 0;

    // A burst of raw toggles inside the debounce window, then a settle -
    // only the final, stable transition may produce an event.
    f.startInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += 5;
    f.startInput.pressed = false;
    f.coordinator.update(t, false, 0);
    t += 5;
    f.startInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    DevCallSessionOutcome startOutcome = f.coordinator.update(t, false, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(startOutcome.kind));
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size()));

    clearLockout(t);
    f.endInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += 5;
    f.endInput.pressed = false;
    f.coordinator.update(t, false, 0);
    t += 5;
    f.endInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    DevCallSessionOutcome endOutcome = f.coordinator.update(t, false, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByButton), static_cast<int>(endOutcome.kind));
    TEST_ASSERT_EQUAL(2, static_cast<int>(f.outbox.size()));
}

// 8. Holding either GPIO HIGH produces exactly one event, never a repeat.
void test_holding_either_gpio_high_does_not_repeat() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome started = pressStart(f, t);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(started.kind));
    for (uint32_t held = t + 1; held < t + 2000; held += 50) {
        DevCallSessionOutcome outcome = f.coordinator.update(held, false, 0);
        TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::None), static_cast<int>(outcome.kind));
    }
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size()));
    t += 2000;

    clearLockout(t);
    DevCallSessionOutcome ended = pressEnd(f, t);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByButton), static_cast<int>(ended.kind));
    for (uint32_t held = t + 1; held < t + 2000; held += 50) {
        DevCallSessionOutcome outcome = f.coordinator.update(held, false, 0);
        TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::None), static_cast<int>(outcome.kind));
    }
    TEST_ASSERT_EQUAL(2, static_cast<int>(f.outbox.size()));
}

// 9. GPIO3 and the safety timeout racing in the same tick still produce
// exactly one RING_ENDED: a qualifying button edge is resolved before the
// timeout check ever runs, so the two can never both fire for one session.
void test_gpio3_and_timeout_racing_produce_only_one_ring_ended() {
    constexpr uint32_t kShortTimeoutMs = 300;
    Fixture f(kShortTimeoutMs);
    uint32_t t = 0;

    DevCallSessionOutcome started = pressStart(f, t);
    const uint32_t callStartedAtMs = t; // the confirming update() call's own nowMs

    // Arrange the end button's own debounce-confirming update() call to
    // land exactly at/after the timeout deadline, so both conditions are
    // simultaneously true going into that single update() call. No
    // lockout to clear here - this is the end button's very first press,
    // and time must keep moving strictly forward (DevRingButtonController
    // assumes monotonic nowMs), so this deliberately does not call
    // clearLockout()/rewind t.
    TEST_ASSERT_TRUE(callStartedAtMs + kShortTimeoutMs > kDevRingButtonDebounceMs + 10);
    t = callStartedAtMs + kShortTimeoutMs - (kDevRingButtonDebounceMs + 10);
    f.endInput.pressed = true;
    f.coordinator.update(t, false, 0);
    t += kDevRingButtonDebounceMs + 10;
    TEST_ASSERT_TRUE(t >= callStartedAtMs + kShortTimeoutMs);
    DevCallSessionOutcome raced = f.coordinator.update(t, false, 0);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByButton), static_cast<int>(raced.kind));
    TEST_ASSERT_EQUAL_STRING(started.callId.c_str(), raced.callId.c_str());
    TEST_ASSERT_EQUAL(2, static_cast<int>(f.outbox.size()));

    // A later call, well past the same deadline, must not also fire a
    // timeout-based RING_ENDED for a session that has already ended.
    DevCallSessionOutcome later = f.coordinator.update(t + 5000, false, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::None), static_cast<int>(later.kind));
    TEST_ASSERT_EQUAL(2, static_cast<int>(f.outbox.size()));
}

// 10. The safety timeout ends the correct (active) session.
void test_timeout_ends_the_active_session() {
    constexpr uint32_t kShortTimeoutMs = 500;
    Fixture f(kShortTimeoutMs);
    uint32_t t = 0;

    DevCallSessionOutcome started = pressStart(f, t);
    const uint32_t callStartedAtMs = t;

    DevCallSessionOutcome timedOut = f.coordinator.update(callStartedAtMs + kShortTimeoutMs, false, 0);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByTimeout), static_cast<int>(timedOut.kind));
    TEST_ASSERT_EQUAL_STRING(started.callId.c_str(), timedOut.callId.c_str());
    TEST_ASSERT_TRUE(matchesEventIdFormat(timedOut.eventId));
    TEST_ASSERT_TRUE(timedOut.eventId != started.eventId);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallState::Idle), static_cast<int>(f.coordinator.state()));
    TEST_ASSERT_EQUAL(2, static_cast<int>(f.outbox.size()));

    auto pending = f.outbox.pending();
    TEST_ASSERT_TRUE(pending[1].eventJson.find("\"event\":\"RING_ENDED\"") != std::string::npos);
    // The timeout reason is deliberately local-log-only (see
    // dev_ring_simulator_main.cpp) - the wire payload must stay
    // indistinguishable from a button-triggered RING_ENDED.
    TEST_ASSERT_TRUE(pending[1].eventJson.find("reason") == std::string::npos);
    TEST_ASSERT_TRUE(pending[1].eventJson.find("timeout") == std::string::npos);
}

// 11. A later call, after a clean end, gets a brand-new call_id.
void test_new_call_after_end_gets_a_new_call_id() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome firstStart = pressStart(f, t);
    clearLockout(t);
    DevCallSessionOutcome firstEnd = pressEnd(f, t);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingEndedByButton), static_cast<int>(firstEnd.kind));

    clearLockout(t);
    releaseStart(f, t); // release GPIO4 so the next press is a fresh edge
    clearLockout(t);
    DevCallSessionOutcome secondStart = pressStart(f, t);

    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(secondStart.kind));
    TEST_ASSERT_TRUE(matchesCallIdFormat(secondStart.callId));
    TEST_ASSERT_TRUE(secondStart.callId != firstStart.callId);
    TEST_ASSERT_EQUAL(3, static_cast<int>(f.outbox.size()));
}

// timestamp only set when clock is valid - same contract as every other
// DeviceEvent producer (see protocol/messages.h).
void test_timestamp_only_set_when_clock_is_valid() {
    Fixture withClock;
    uint32_t t1 = 0;
    withClock.startInput.pressed = true;
    withClock.coordinator.update(t1, true, 1755000000);
    t1 += kDevRingButtonDebounceMs + 10;
    DevCallSessionOutcome outcomeValid = withClock.coordinator.update(t1, true, 1755000000);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(outcomeValid.kind));
    auto pendingValid = withClock.outbox.pending();
    TEST_ASSERT_TRUE(pendingValid[0].eventJson.find("\"timestamp\":\"") != std::string::npos);

    Fixture withoutClock;
    uint32_t t2 = 0;
    withoutClock.startInput.pressed = true;
    withoutClock.coordinator.update(t2, false, 1755000000);
    t2 += kDevRingButtonDebounceMs + 10;
    DevCallSessionOutcome outcomeInvalid = withoutClock.coordinator.update(t2, false, 1755000000);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(outcomeInvalid.kind));
    auto pendingInvalid = withoutClock.outbox.pending();
    TEST_ASSERT_TRUE(pendingInvalid[0].eventJson.find("timestamp") == std::string::npos);
}

// 12 + 13. Retry preserves event_id/call_id verbatim, AND the outbox
// publishes strictly in enqueue order: a RING_ENDED already sitting behind
// its own RING_DETECTED must never be published first just because a
// retry attempt for the RING_DETECTED happens to fail. Before this pass,
// publishPendingEvents() kept iterating past a failed entry - this test
// would have failed against that version, since armPublishFailure(1) only
// fails the very first publish() call (the RING_DETECTED) and would have
// let the RING_ENDED behind it succeed immediately.
void test_retry_preserves_ids_and_ring_detected_publishes_before_ring_ended() {
    MemoryEventOutbox outbox;
    const std::string callId = std::string("call-") + std::string(32, '1');
    const std::string detectedEventId = std::string("evt-") + std::string(32, '2');
    const std::string endedEventId = std::string("evt-") + std::string(32, '3');
    const std::string detectedPayload = std::string(R"({"protocol_version":1,"device_id":")") + kTestDeviceId +
                                        R"(","event":"RING_DETECTED","event_id":")" + detectedEventId +
                                        R"(","call_id":")" + callId + R"("})";
    const std::string endedPayload = std::string(R"({"protocol_version":1,"device_id":")") + kTestDeviceId +
                                     R"(","event":"RING_ENDED","event_id":")" + endedEventId + R"(","call_id":")" +
                                     callId + R"("})";
    outbox.enqueue(detectedEventId, detectedPayload);
    outbox.enqueue(endedEventId, endedPayload);

    FakeDeviceTransport transport;
    transport.connect(kTestDeviceId);
    transport.armPublishFailure(1); // only the very first publish() call fails

    size_t publishedFirst = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(0, static_cast<int>(publishedFirst));
    TEST_ASSERT_EQUAL(2, static_cast<int>(outbox.size()));

    auto pendingAfterFailure = outbox.pending();
    TEST_ASSERT_EQUAL(2, static_cast<int>(pendingAfterFailure.size()));
    TEST_ASSERT_EQUAL_STRING(detectedEventId.c_str(), pendingAfterFailure[0].eventId.c_str());
    TEST_ASSERT_EQUAL_STRING(endedEventId.c_str(), pendingAfterFailure[1].eventId.c_str());
    TEST_ASSERT_EQUAL(0, static_cast<int>(transport.publishedMessages().size()));

    size_t publishedSecond = publishPendingEvents(outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(2, static_cast<int>(publishedSecond));
    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));

    const auto& published = transport.publishedMessages();
    TEST_ASSERT_EQUAL(2, static_cast<int>(published.size()));
    TEST_ASSERT_EQUAL_STRING(detectedPayload.c_str(), published[0].payload.c_str());
    TEST_ASSERT_EQUAL_STRING(endedPayload.c_str(), published[1].payload.c_str());
}

// 14. A fresh coordinator instance (mirroring a firmware restart, since
// this DEV composition is RAM-only and reconstructed from scratch at
// boot - see dev_ring_simulator_main.cpp) always starts Idle; there is no
// mechanism that restores a previously-active simulated call.
void test_new_coordinator_instance_starts_idle() {
    Fixture f;
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallState::Idle), static_cast<int>(f.coordinator.state()));
    TEST_ASSERT_TRUE(f.coordinator.activeCallId().empty());
}

void test_offline_press_enqueues_and_reconnect_replays() {
    Fixture f;
    uint32_t t = 0;

    DevCallSessionOutcome outcome = pressStart(f, t);
    TEST_ASSERT_EQUAL(static_cast<int>(DevCallSessionEventKind::RingDetected), static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size())); // enqueued with no transport involved at all

    FakeDeviceTransport transport; // never connected: simulates being offline
    TEST_ASSERT_FALSE(transport.isConnected());
    TEST_ASSERT_EQUAL(1, static_cast<int>(f.outbox.size())); // still queued while offline

    transport.connect(kTestDeviceId); // reconnect
    size_t published = publishPendingEvents(f.outbox, transport, "topic/events");
    TEST_ASSERT_EQUAL(1, static_cast<int>(published));
    TEST_ASSERT_EQUAL(0, static_cast<int>(f.outbox.size()));

    const auto& publishedMessages = transport.publishedMessages();
    TEST_ASSERT_EQUAL(1, static_cast<int>(publishedMessages.size()));
    TEST_ASSERT_TRUE(publishedMessages[0].payload.find(outcome.eventId) != std::string::npos);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_device_id_fixture_is_valid);
    RUN_TEST(test_gpio4_in_idle_creates_session_and_ring_detected);
    RUN_TEST(test_ring_detected_event_id_and_call_id_are_distinct);
    RUN_TEST(test_gpio3_reuses_the_same_call_id_in_ring_ended);
    RUN_TEST(test_ring_detected_and_ring_ended_have_different_event_ids);
    RUN_TEST(test_gpio3_in_idle_is_ignored);
    RUN_TEST(test_gpio4_while_ringing_is_ignored);
    RUN_TEST(test_debounce_prevents_duplicate_on_both_gpios);
    RUN_TEST(test_holding_either_gpio_high_does_not_repeat);
    RUN_TEST(test_gpio3_and_timeout_racing_produce_only_one_ring_ended);
    RUN_TEST(test_timeout_ends_the_active_session);
    RUN_TEST(test_new_call_after_end_gets_a_new_call_id);
    RUN_TEST(test_timestamp_only_set_when_clock_is_valid);
    RUN_TEST(test_retry_preserves_ids_and_ring_detected_publishes_before_ring_ended);
    RUN_TEST(test_new_coordinator_instance_starts_idle);
    RUN_TEST(test_offline_press_enqueues_and_reconnect_replays);
    return UNITY_END();
}
