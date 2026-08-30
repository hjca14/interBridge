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

// Wi-Fi association is asynchronous (see the wifiAssociationResult() tests
// below), so - unlike before the fix - a bare failed update() no longer
// advances the backoff by itself; each attempt must be explicitly resolved
// via wifiAssociationResult() first, exactly as a real disconnect event
// would resolve it.
void test_backoff_is_capped_and_deadline_wrap_is_safe() {
    DevMqttSmokeState state(10, 20);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));
    state.wifiAssociationResult(0, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(10, false, false, false)));
    state.wifiAssociationResult(10, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(30, false, false, false)));
    TEST_ASSERT_EQUAL_UINT32(20, state.retryDelayMs());
    TEST_ASSERT_FALSE(DevMqttSmokeState::deadlineReached(0xfffffff0u, 0x00000005u));
    TEST_ASSERT_TRUE(DevMqttSmokeState::deadlineReached(0x00000006u, 0x00000005u));
}

// millisUntil() is a diagnostic-logging-only helper (see its declaration) -
// never used for any retry/backoff decision. Real hardware showed the naive
// "deadlineMs - nowMs" computation underflow to delay_ms=4294967291/4294967294
// once ConnectWifi started being logged right at (or a few ms after) its own
// already-elapsed backoff deadline - which is guaranteed by construction the
// moment that action fires (deadlineReached() must already be true). Must
// saturate at 0 instead, and stay correct across the millis() wraparound.
void test_millis_until_saturates_and_is_wrap_safe() {
    // Ordinary future deadline: 5000ms remaining.
    TEST_ASSERT_EQUAL_UINT32(5000, DevMqttSmokeState::millisUntil(15000, 10000));

    // Deadline already reached - exactly now, or a few ms in the past -
    // must saturate at 0, never underflow via plain unsigned subtraction.
    TEST_ASSERT_EQUAL_UINT32(0, DevMqttSmokeState::millisUntil(10000, 10000));
    TEST_ASSERT_EQUAL_UINT32(0, DevMqttSmokeState::millisUntil(10000, 10003));

    // Wraparound, deadline in the past: the deadline (0xFFFFFFFE) is 5ms
    // before "now" (3) once the wrap is accounted for, even though "now"
    // numerically wrapped past 0 - must still saturate at 0, not report a
    // huge number.
    TEST_ASSERT_EQUAL_UINT32(0, DevMqttSmokeState::millisUntil(0xFFFFFFFEu, 3u));

    // Wraparound, deadline in the future: the deadline (3) is genuinely
    // 5ms after "now" (0xFFFFFFFE, 2ms before the wrap) once the wrap is
    // accounted for - must report the real 5ms remaining, not treat the
    // wrap as "already past".
    TEST_ASSERT_EQUAL_UINT32(5, DevMqttSmokeState::millisUntil(3u, 0xFFFFFFFEu));
}

// Wi-Fi association itself is asynchronous on real hardware, exactly like
// NTP/SNTP - the ESP32 Wi-Fi driver actively rejects (and effectively
// restarts) a WiFi.begin() call issued while a previous attempt is still
// outstanding ("wifi:sta is connecting, return error" / "WiFiSTA.cpp
// begin(): connect failed!" observed on real hardware). The bare fact that
// wifiConnected is not yet true must never by itself authorize reissuing
// ConnectWifi - see update()'s contract and wifiAssociationResult().
void test_connect_wifi_issued_once_while_association_pending() {
    DevMqttSmokeState state(10, 40, 15000, 3, 600000, /*wifiAssociationTimeoutMs=*/1000);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    // Many rapid updates while the attempt is pending - well before its own
    // 1000ms timeout, and past what the ordinary 10ms initial backoff would
    // have allowed under the pre-fix behavior - must never reissue
    // ConnectWifi.
    int connectWifiCount = 0;
    for (uint32_t t = 1; t < 900; t += 10) {
        if (state.update(t, false, false, false) == DevSmokeAction::ConnectWifi) {
            ++connectWifiCount;
        }
    }
    TEST_ASSERT_EQUAL(0, connectWifiCount);
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());
}

// An explicit success signal (forwarded from a real
// ARDUINO_EVENT_WIFI_STA_CONNECTED/GOT_IP event) may arrive before
// wifiConnected itself is observed true; it only needs to stop treating the
// attempt as in flight. The ordinary wifiConnected-driven cascade is still
// what actually advances the state, unchanged from before this fix.
void test_wifi_association_success_ends_attempt_and_advances_normally() {
    DevMqttSmokeState state(10, 40);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    state.wifiAssociationResult(1, true);
    TEST_ASSERT_FALSE(state.wifiAttemptInFlight());

    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns),
                      static_cast<int>(state.update(2, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForDns), static_cast<int>(state.state()));
}

// A real failure signal (forwarded from an
// ARDUINO_EVENT_WIFI_STA_DISCONNECTED event) ends the attempt and schedules
// the next retry immediately - not at the moment ConnectWifi was originally
// issued - but that retry still must not fire before its own backoff
// deadline elapses.
void test_wifi_association_failure_ends_attempt_and_schedules_retry() {
    DevMqttSmokeState state(10, 40);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    state.wifiAssociationResult(1, false);
    TEST_ASSERT_FALSE(state.wifiAttemptInFlight());

    // No attempt occurs before the backoff elapses (scheduled from t=1 with
    // initialRetryMs=10 -> next attempt not before t=11).
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(10, false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(11, false, false, false)));
}

// If neither a connected/got_ip nor a disconnected event ever arrives, the
// attempt's own bounded timeout abandons it and schedules a retry, gated on
// the ordinary backoff deadline exactly like the NTP attempt timeout.
void test_wifi_association_timeout_ends_stuck_attempt_and_allows_later_retry() {
    DevMqttSmokeState state(10, 40, 15000, 3, 600000, /*wifiAssociationTimeoutMs=*/5);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    // Flight deadline is 0+5=5 - before it, still suppressed, with no
    // explicit event ever arriving.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(4, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    // At the flight deadline, the attempt is abandoned and a retry is
    // scheduled from this moment (deadline 5 + initialRetryMs 10 = 15) -
    // not from the original issue time.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(5, false, false, false)));
    TEST_ASSERT_FALSE(state.wifiAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(10, false, false, false)));
    TEST_ASSERT_FALSE(state.wifiAttemptInFlight());

    // Once that backoff deadline (15) is reached, exactly one fresh retry
    // fires and re-arms the flight timer.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(15, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());
}

// The association flight timeout deadline reuses the same wrap-safe
// deadlineReached() comparison as the rest of the state machine - mirrors
// test_ntp_attempt_timeout_is_wrap_safe's own bootstrap-then-force-the-
// wrap-transition shape (see that test's comment for why jumping straight
// from a small bootstrap value to one near the wrap, without first seeding
// retryAtMs_ from a nearby timestamp via a real state transition, would
// otherwise be indistinguishable from going backward in time).
void test_wifi_association_timeout_is_wrap_safe() {
    DevMqttSmokeState state(1000, 4000, 15000, 3, 600000, /*wifiAssociationTimeoutMs=*/20);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, true, false);
    state.mqttResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));

    const uint32_t nearWrap = 0xFFFFFFF0u; // 16 before millis() would wrap to 0
    // Forcing a Wi-Fi loss right at nowMs=nearWrap makes enter(WaitingForWifi,
    // nearWrap) seed retryAtMs_ from a value consistent with the
    // near-the-wrap timestamps used below.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(nearWrap, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    // Flight deadline (nearWrap + 20) wraps past 0xFFFFFFFF to 4.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 19, false, false, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 20, false, false, false)));
    TEST_ASSERT_FALSE(state.wifiAttemptInFlight());
}

// The existing Wi-Fi interface recovery ladder (RecoverWifi) must keep
// working with the new in-flight tracking: the ConnectWifi it authorizes
// once the real disconnect is observed is protected from reissue exactly
// like an ordinary attempt.
void test_wifi_recovery_reconnect_attempt_is_also_protected_from_reissue() {
    DevMqttSmokeState state(10, 40, 1000, /*wifiRecoveryThreshold=*/1, /*wifiRecoveryCooldownMs=*/1000,
                            /*wifiAssociationTimeoutMs=*/1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));
    state.mqttResult(1, false); // single failure reaches threshold=1
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::RecoverWifi),
                      static_cast<int>(state.update(1, true, true, false)));

    // The real disconnect is observed, releasing ConnectWifi.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(2, false, true, false)));
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());

    // The freshly re-armed attempt is protected exactly like an ordinary
    // one - rapid updates before it resolves must not reissue it.
    for (uint32_t t = 3; t < 900; t += 10) {
        TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                          static_cast<int>(state.update(t, false, true, false)));
    }
    TEST_ASSERT_TRUE(state.wifiAttemptInFlight());
}

// A burst of multiple disconnect events for the very same attempt (real
// hardware can emit more than one) must only ever count as a single
// failure/backoff - never re-arm the timer again or otherwise cause more
// than one ConnectWifi to fire at the single scheduled deadline.
void test_repeated_wifi_association_failure_signals_do_not_cause_a_connect_storm() {
    DevMqttSmokeState state(10, 40);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(0, false, false, false)));

    state.wifiAssociationResult(1, false);
    uint32_t retryAtAfterFirstFailure = state.retryAtMs();
    uint32_t retryDelayAfterFirstFailure = state.retryDelayMs();
    state.wifiAssociationResult(1, false);
    state.wifiAssociationResult(1, false);
    TEST_ASSERT_EQUAL_UINT32(retryAtAfterFirstFailure, state.retryAtMs());
    TEST_ASSERT_EQUAL_UINT32(retryDelayAfterFirstFailure, state.retryDelayMs());

    int connectWifiCount = 0;
    for (uint32_t t = 2; t <= retryAtAfterFirstFailure + 5; ++t) {
        if (state.update(t, false, false, false) == DevSmokeAction::ConnectWifi) {
            ++connectWifiCount;
        }
    }
    TEST_ASSERT_EQUAL(1, connectWifiCount);
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
    TEST_ASSERT_TRUE(state.awaitingWifiRecoveryDisconnect());

    // WiFi.disconnect() is asynchronous: wifiConnected can still read true
    // for a tick or more. Until the real disconnect is observed, the
    // cascade must not advance past WaitingForWifi - it must not jump
    // straight to ResolveDns/ConnectMqtt over the stale association.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(32, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
    TEST_ASSERT_TRUE(state.awaitingWifiRecoveryDisconnect());

    // Only once wifiConnected genuinely reports false does the ordinary
    // ConnectWifi (WiFi.begin()) cascade fire - the forced re-association
    // this recovery exists for.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(40, false, true, false)));
    TEST_ASSERT_FALSE(state.awaitingWifiRecoveryDisconnect());

    // Recovery requires the ordinary cascade again - never jumps straight
    // back into WaitingForMqtt/ConnectMqtt.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns),
                      static_cast<int>(state.update(45, true, true, false)));
    state.dnsResult(45, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(45, true, true, false)));

    // Three more failures while still inside the 1000ms cooldown (armed at
    // t=31, so active through t=1031) must not trigger a second recovery,
    // no matter how many accumulate - the ordinary backoff keeps retrying.
    state.mqttResult(45, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(60, true, true, false)));
    state.mqttResult(60, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(100, true, true, false)));
    state.mqttResult(100, false); // 3rd failure of this new run, still cooling down
    TEST_ASSERT_EQUAL(0, state.consecutiveConnectivityFailures());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(100, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForMqtt), static_cast<int>(state.state()));
}

// Explicit regression test for the WiFi.disconnect() async race: after
// RecoverWifi is issued, wifiConnected can still (falsely) read true for a
// tick or more, and the cascade must never advance past WaitingForWifi
// during that window - it must wait for the real disconnect signal before
// letting ConnectWifi (WiFi.begin()) fire, and only then resume the
// ordinary ResolveDns/.../ConnectMqtt flow.
void test_wifi_recovery_waits_for_real_disconnect_before_resuming_cascade() {
    DevMqttSmokeState state(10, 40, 1000, /*wifiRecoveryThreshold=*/1, /*wifiRecoveryCooldownMs=*/1000);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(1, true, true, false)));

    // 1. A single failure reaches the (deliberately low, for this test)
    // threshold and RecoverWifi is issued.
    state.mqttResult(1, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::RecoverWifi),
                      static_cast<int>(state.update(1, true, true, false)));
    TEST_ASSERT_TRUE(state.awaitingWifiRecoveryDisconnect());

    // 2. WiFi.disconnect() is asynchronous - simulate the very next read
    // still (falsely) reporting connected. The action must be None and the
    // state must not advance to WaitingForDns/ResolveDns or ConnectMqtt.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(2, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
    // 3. The same false-positive read can repeat for more than one tick.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(3, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));

    // 4/5. Once wifiConnected genuinely reports false, the ordinary
    // ConnectWifi (WiFi.begin()) cascade fires - the forced re-association
    // this recovery exists for.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(4, false, true, false)));
    TEST_ASSERT_FALSE(state.awaitingWifiRecoveryDisconnect());

    // 6. WiFi/DNS/time return and the ordinary flow resumes normally,
    // eventually reaching ConnectMqtt again.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns),
                      static_cast<int>(state.update(5, true, true, false)));
    state.dnsResult(5, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(5, true, true, false)));
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

    // The disconnect must be observed (wifiConnected=false) before the
    // cascade is allowed to resume - see
    // test_wifi_recovery_waits_for_real_disconnect_before_resuming_cascade.
    // This also exercises that gate's own deadline math right at the wrap.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi),
                      static_cast<int>(state.update(nearWrap + 1, false, true, false)));

    // Re-bootstrap to WaitingForMqtt again while still inside the wrapped
    // cooldown window.
    state.update(nearWrap + 2, true, false, false);
    state.dnsResult(nearWrap + 2, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                      static_cast<int>(state.update(nearWrap + 2, true, true, false)));
    state.mqttResult(nearWrap + 2, false); // another failure, still cooling down
    TEST_ASSERT_TRUE(state.wifiRecoveryCooldownActive());
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(nearWrap + 2, true, true, false)));

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
    RUN_TEST(test_millis_until_saturates_and_is_wrap_safe);
    RUN_TEST(test_connect_wifi_issued_once_while_association_pending);
    RUN_TEST(test_wifi_association_success_ends_attempt_and_advances_normally);
    RUN_TEST(test_wifi_association_failure_ends_attempt_and_schedules_retry);
    RUN_TEST(test_wifi_association_timeout_ends_stuck_attempt_and_allows_later_retry);
    RUN_TEST(test_wifi_association_timeout_is_wrap_safe);
    RUN_TEST(test_wifi_recovery_reconnect_attempt_is_also_protected_from_reissue);
    RUN_TEST(test_repeated_wifi_association_failure_signals_do_not_cause_a_connect_storm);
    RUN_TEST(test_ntp_attempt_marks_in_flight_and_suppresses_reissue_until_timeout);
    RUN_TEST(test_ntp_sync_completion_clears_in_flight_and_continues_to_mqtt);
    RUN_TEST(test_ntp_attempt_timeout_allows_exactly_one_fresh_retry);
    RUN_TEST(test_ntp_attempt_timeout_is_wrap_safe);
    RUN_TEST(test_ntp_attempt_in_flight_does_not_reissue_across_many_rapid_updates);
    RUN_TEST(test_network_preflight_failure_returns_to_dns_without_reexecuting_ntp);
    RUN_TEST(test_repeated_connectivity_failures_trigger_wifi_recovery_then_respect_cooldown);
    RUN_TEST(test_wifi_recovery_waits_for_real_disconnect_before_resuming_cascade);
    RUN_TEST(test_full_mqtt_success_resets_connectivity_counters_and_recovery_state);
    RUN_TEST(test_wifi_recovery_cooldown_deadline_is_wrap_safe);
    RUN_TEST(test_connectivity_counter_is_untouched_by_a_normal_successful_cycle);
    RUN_TEST(test_observation_only_update_does_not_change_online_state);
    return UNITY_END();
}
