#include <unity.h>

#include "dev/mqtt_smoke_state.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_happy_path_and_time_gate() {
    DevMqttSmokeState state(10, 40);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi), static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(1, true, false, false)));
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime), static_cast<int>(state.update(1, true, false, false)));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                          static_cast<int>(state.update(10, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(11, true, true, false)));
    state.mqttResult(11, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

void test_dns_and_mqtt_failures_back_off_and_recover() {
    DevMqttSmokeState state(10, 40);
    state.update(0, false, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(1, true, false, false)));
    state.dnsResult(1, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(10, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(11, true, false, false)));
    state.dnsResult(11, true);
    state.update(11, true, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(21, true, true, false)));
    state.mqttResult(21, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(30, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(31, true, true, false)));
    state.mqttResult(31, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

void test_wifi_loss_requires_all_gates_again() {
    DevMqttSmokeState state(10, 40);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, false, false);
    state.update(11, true, true, false);
    state.mqttResult(11, true);
    state.update(12, false, true, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(13, true, true, false)));
}

void test_backoff_is_capped_and_deadline_wrap_is_safe() {
    DevMqttSmokeState state(10, 20);
    state.update(0, false, false, false);
    state.update(10, false, false, false);
    state.update(30, false, false, false);
    TEST_ASSERT_EQUAL_UINT32(20, state.retryDelayMs());
    TEST_ASSERT_FALSE(DevMqttSmokeState::deadlineReached(0xfffffff0u, 0x00000005u));
    TEST_ASSERT_TRUE(DevMqttSmokeState::deadlineReached(0x00000006u, 0x00000005u));
}

// First ConfigureTime marks the attempt in flight, and it is never reissued
// on subsequent updates before either the real completion signal (timeValid)
// or the attempt's own bounded timeout - not gated on any external
// "sntp in progress" signal, which real hardware showed can stay
// reset/idle for a while after configTime() is called.
void test_ntp_attempt_marks_in_flight_and_suppresses_reissue_until_timeout() {
    DevMqttSmokeState state(10, 40, 100); // ntpAttemptTimeoutMs=100, well past the backoff deadline
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_FALSE(state.ntpAttemptInFlight());

    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(1, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());

    // The ordinary backoff deadline (t=11) elapses well before the 100ms
    // flight timeout - must not reissue ConfigureTime regardless.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(11, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(50, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());
}

// The real completion signal (timeValid becoming true, driven by the
// caller's own sync-complete callback) clears the in-flight attempt and
// lets the state machine continue into WaitingForMqtt in the same update()
// call - the same "transition then try the next action immediately" shape
// used elsewhere in this state machine (see test_happy_path_and_time_gate).
void test_ntp_sync_completion_clears_in_flight_and_continues_to_mqtt() {
    DevMqttSmokeState state(10, 40, 100);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, false, false); // issues ConfigureTime, marks in flight
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());

    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(60, true, true, false)));
    TEST_ASSERT_FALSE(state.ntpAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForMqtt),
                      static_cast<int>(state.state()));
}

// If the flight timeout elapses without ever seeing timeValid become true,
// the attempt is treated as failed and exactly one fresh ConfigureTime retry
// is allowed - gated on the ordinary backoff deadline also having elapsed by
// then (the timeout release does not bypass the backoff floor).
void test_ntp_attempt_timeout_allows_exactly_one_fresh_retry() {
    DevMqttSmokeState state(10, 40, 5); // ntpAttemptTimeoutMs=5, short relative to backoff
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(1, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());

    // Flight deadline is 1+5=6 - before it, still suppressed.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(5, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());

    // At the flight deadline, the attempt is abandoned, but the ordinary
    // backoff deadline (t=11) has not been reached yet, so no new attempt
    // is issued immediately.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(6, true, false, false)));
    TEST_ASSERT_FALSE(state.ntpAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(10, true, false, false)));
    TEST_ASSERT_FALSE(state.ntpAttemptInFlight());

    // Once the backoff deadline is also reached, exactly one fresh retry
    // fires and re-arms the flight timer.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(11, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());
}

// The flight timeout deadline reuses the same wrap-safe deadlineReached()
// comparison as the rest of the state machine.
//
// deadlineReached() only gives the intuitively-correct answer when the two
// timestamps being compared are already "close" to each other on the 32-bit
// circle (within ~2^31) - jumping nowMs from a small value straight to one
// near the wrap while comparing it against an unrelated small deadline
// (e.g. the constructor's default retryAtMs_ = 0) is indistinguishable from
// going backward in time and is misinterpreted accordingly. That is a
// property of the wraparound scheme itself (millis() in reality only ever
// advances by small increments), not something to work around: the fix is
// to keep every timestamp in this test consistently near the wrap instead
// of jumping from small bootstrap values.
void test_ntp_attempt_timeout_is_wrap_safe() {
    // A much larger backoff ceiling than the flight timeout, so the
    // ordinary backoff deadline stays comfortably in the future relative to
    // the flight timeout - keeps this test isolated to only the flight
    // timeout's own wrap-safety, without a coincidentally-also-elapsed
    // backoff deadline immediately re-arming a new attempt in the same call.
    DevMqttSmokeState state(1000, 4000, 20);
    const uint32_t nearWrap = 0xFFFFFFF0u; // 16 before millis() would wrap to 0
    // wifiConnected=true from the very first call skips the WaitingForWifi
    // retry check (which would otherwise compare nearWrap against the
    // constructor's small default retryAtMs_ = 0 - an unrelated corner this
    // test isn't about) and lets enter() seed retryAtMs_ directly from
    // nearWrap instead.
    state.update(nearWrap, true, false, false);
    state.dnsResult(nearWrap, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(nearWrap, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());

    // Flight deadline (nearWrap + 20) wraps past 0xFFFFFFFF to 4; the
    // ordinary backoff deadline (nearWrap + 1000) also wraps, to 984 - far
    // enough past 4 that it plays no part in what this test checks.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 19, true, false, false)));
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 20, true, false, false)));
    TEST_ASSERT_FALSE(state.ntpAttemptInFlight());
}

// Simulates ~90 rapid 10ms main-loop ticks while an attempt is in flight -
// proves this cannot degrade into a tight configTime() retry loop.
void test_ntp_attempt_in_flight_does_not_reissue_across_many_rapid_updates() {
    DevMqttSmokeState state(10, 40, 1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(1, true, false, false)));

    int configureTimeCount = 0;
    for (uint32_t t = 2; t < 900; t += 10) {
        if (state.update(t, true, false, false) == DevSmokeAction::ConfigureTime) {
            ++configureTimeCount;
        }
    }
    TEST_ASSERT_EQUAL(0, configureTimeCount);
    TEST_ASSERT_TRUE(state.ntpAttemptInFlight());
}

// A stale WL_CONNECTED Wi-Fi association does not prove DNS is actually
// working (real-hardware finding: a device stayed WL_CONNECTED for ~110 min
// then lost DNS/TLS entirely, and the state machine never returned to
// WaitingForDns). networkPreflightFailed() reports the caller's own DNS
// preflight check (performed before ever attempting transport.connect())
// failing: no connect() attempt should have been made, and the state
// machine must fall back to WaitingForDns using that stage's own backoff.
// Once DNS recovers, MQTT is retried without re-running NTP (timeValid is
// still true throughout).
void test_network_preflight_failure_returns_to_dns_without_reexecuting_ntp() {
    DevMqttSmokeState state(10, 40, 1000, 3, 100000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForMqtt), static_cast<int>(state.state()));

    // Preflight fails - no transport.connect() attempt was made by the
    // caller for this cycle.
    state.networkPreflightFailed(1);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForDns), static_cast<int>(state.state()));
    TEST_ASSERT_EQUAL(1, state.consecutiveConnectivityFailures());

    // The very next update() issues ResolveDns, never ConnectMqtt directly.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns),
                      static_cast<int>(state.update(2, true, true, false)));

    // DNS recovers - timeValid is still true throughout, so ConfigureTime
    // must not be reissued; the next update() goes straight back to MQTT.
    state.dnsResult(2, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(2, true, true, false)));
}

// Three consecutive connectivity failures (DNS preflight and/or TLS/socket
// MQTT connect - never publish failures, which have their own unrelated
// outbox flow in RemoteCommandProcessor/Esp32AwsIotTransport) authorize
// exactly one Wi-Fi interface recovery. A recovery in turn requires the
// ordinary ConnectWifi -> ResolveDns -> ... cascade again (never jumps
// straight back to ConnectMqtt), and further failures while a cooldown is
// active never trigger a second recovery - the ordinary per-stage backoff
// keeps retrying on its own instead. Once the cooldown elapses, a fresh
// failure is authorized to recover again.
void test_repeated_connectivity_failures_trigger_wifi_recovery_then_respect_cooldown() {
    DevMqttSmokeState state(10, 40, 1000, /*wifiRecoveryThreshold=*/3, /*wifiRecoveryCooldownMs=*/1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));

    // Failure 1/3.
    state.mqttResult(1, false);
    TEST_ASSERT_EQUAL(1, state.consecutiveConnectivityFailures());
    TEST_ASSERT_FALSE(state.wifiRecoveryCooldownActive());

    // Failure 2/3.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(11, true, true, false)));
    state.mqttResult(11, false);
    TEST_ASSERT_EQUAL(2, state.consecutiveConnectivityFailures());
    // Still below threshold: no recovery, ordinary backoff continues.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(11, true, true, false)));

    // Failure 3/3 authorizes exactly one Wi-Fi recovery.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(31, true, true, false)));
    state.mqttResult(31, false);
    TEST_ASSERT_EQUAL(0, state.consecutiveConnectivityFailures()); // reset on reaching the threshold
    TEST_ASSERT_TRUE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::RecoverWifi),
                      static_cast<int>(state.update(31, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));

    // Recovery requires the ordinary cascade again - never jumps straight
    // back into WaitingForMqtt/ConnectMqtt.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns),
                      static_cast<int>(state.update(32, true, true, false)));
    state.dnsResult(32, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(32, true, true, false)));

    // Three more failures while still inside the 1000ms cooldown (armed at
    // t=31, so active through t=1031) must not trigger a second recovery,
    // no matter how many accumulate - the ordinary backoff keeps retrying.
    state.mqttResult(32, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(42, true, true, false)));
    state.mqttResult(42, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(62, true, true, false)));
    state.mqttResult(62, false); // 3rd failure of this new run, still cooling down
    TEST_ASSERT_EQUAL(0, state.consecutiveConnectivityFailures());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(62, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForMqtt), static_cast<int>(state.state()));
}

// A full MQTT connect+subscribe success is the strongest possible health
// signal - it clears both the connectivity failure counter and any active
// Wi-Fi recovery cooldown, so a later, unrelated outage starts counting
// from zero rather than inheriting stale state.
void test_full_mqtt_success_resets_connectivity_counters_and_recovery_state() {
    DevMqttSmokeState state(10, 40, 1000, 3, 1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));

    state.mqttResult(1, false);
    TEST_ASSERT_EQUAL(1, state.consecutiveConnectivityFailures());

    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(11, true, true, false)));
    state.mqttResult(11, true); // full success (connect+subscribe)
    TEST_ASSERT_EQUAL(0, state.consecutiveConnectivityFailures());
    TEST_ASSERT_FALSE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

// The Wi-Fi recovery cooldown deadline reuses the same wrap-safe
// deadlineReached() comparison as the rest of the state machine.
// wifiRecoveryThreshold=1 keeps this test focused purely on the cooldown
// deadline itself rather than re-deriving the failure-counting behavior
// already covered above.
void test_wifi_recovery_cooldown_deadline_is_wrap_safe() {
    DevMqttSmokeState state(1000, 4000, 1000, /*wifiRecoveryThreshold=*/1, /*wifiRecoveryCooldownMs=*/20);
    const uint32_t nearWrap = 0xFFFFFFF0u; // 16 before millis() would wrap to 0
    // wifiConnected=true from the very first call, as in
    // test_ntp_attempt_timeout_is_wrap_safe, keeps every timestamp
    // consistently near the wrap instead of jumping from small defaults.
    state.update(nearWrap, true, false, false);
    state.dnsResult(nearWrap, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(nearWrap, true, true, false)));

    // A single failure (threshold=1) immediately authorizes a recovery and
    // arms a 20ms cooldown that wraps past 0xFFFFFFFF (to 4).
    state.mqttResult(nearWrap, false);
    TEST_ASSERT_TRUE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::RecoverWifi),
                      static_cast<int>(state.update(nearWrap, true, true, false)));

    // Re-bootstrap to WaitingForMqtt again while still inside the wrapped
    // cooldown window.
    state.update(nearWrap + 1, true, false, false);
    state.dnsResult(nearWrap + 1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(nearWrap + 1, true, true, false)));
    state.mqttResult(nearWrap + 1, false); // another failure, still cooling down
    TEST_ASSERT_TRUE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 1, true, true, false)));

    // Past the wrapped cooldown deadline (4), a fresh failure is authorized
    // to recover again.
    state.mqttResult(5, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::RecoverWifi),
                      static_cast<int>(state.update(5, true, true, false)));
}

// Regression guard: DevMqttSmokeState has no knowledge of response-publish
// failures at all - RemoteCommandProcessor/Esp32AwsIotTransport handle those
// entirely through their own outbox and session-invalidation flow (see
// docs/architecture.md), never through this class or its Wi-Fi recovery
// ladder. A normal healthy connect+subscribe cycle never touches the
// connectivity failure counter.
void test_connectivity_counter_is_untouched_by_a_normal_successful_cycle() {
    DevMqttSmokeState state(10, 40, 1000, 3, 1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));
    state.mqttResult(1, true);
    TEST_ASSERT_EQUAL(0, state.consecutiveConnectivityFailures());
    TEST_ASSERT_FALSE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

void test_observation_only_update_does_not_change_online_state() {
    DevMqttSmokeState state;
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, true, false);
    state.mqttResult(1, true);
    state.update(50000, true, true, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_and_time_gate);
    RUN_TEST(test_dns_and_mqtt_failures_back_off_and_recover);
    RUN_TEST(test_wifi_loss_requires_all_gates_again);
    RUN_TEST(test_backoff_is_capped_and_deadline_wrap_is_safe);
    RUN_TEST(test_ntp_attempt_marks_in_flight_and_suppresses_reissue_until_timeout);
    RUN_TEST(test_ntp_sync_completion_clears_in_flight_and_continues_to_mqtt);
    RUN_TEST(test_ntp_attempt_timeout_allows_exactly_one_fresh_retry);
    RUN_TEST(test_ntp_attempt_timeout_is_wrap_safe);
    RUN_TEST(test_ntp_attempt_in_flight_does_not_reissue_across_many_rapid_updates);
    RUN_TEST(test_network_preflight_failure_returns_to_dns_without_reexecuting_ntp);
    RUN_TEST(test_repeated_connectivity_failures_trigger_wifi_recovery_then_respect_cooldown);
    RUN_TEST(test_full_mqtt_success_resets_connectivity_counters_and_recovery_state);
    RUN_TEST(test_wifi_recovery_cooldown_deadline_is_wrap_safe);
    RUN_TEST(test_connectivity_counter_is_untouched_by_a_normal_successful_cycle);
    RUN_TEST(test_observation_only_update_does_not_change_online_state);
    return UNITY_END();
}
