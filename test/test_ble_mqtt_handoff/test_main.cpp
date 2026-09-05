#include <unity.h>

#include "dev/ble_mqtt_handoff.h"
#include "dev/mqtt_smoke_state.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

// --- BleMqttHandoffGate: the new orchestration boundary itself ---------

void test_gate_starts_with_ble_owning_wifi() {
    BleMqttHandoffGate gate;
    TEST_ASSERT_TRUE(gate.bleOwnsWifi());
    TEST_ASSERT_FALSE(gate.shouldHandleConnectWifiAction());
}

void test_gate_hands_off_once_provisioning_ends() {
    BleMqttHandoffGate gate;
    gate.markProvisioningEnded();
    TEST_ASSERT_FALSE(gate.bleOwnsWifi());
    TEST_ASSERT_TRUE(gate.shouldHandleConnectWifiAction());
}

void test_gate_hand_off_is_a_one_shot_latch() {
    BleMqttHandoffGate gate;
    gate.markProvisioningEnded();
    gate.markProvisioningEnded();
    gate.markProvisioningEnded();
    TEST_ASSERT_FALSE(gate.bleOwnsWifi());
    TEST_ASSERT_TRUE(gate.shouldHandleConnectWifiAction());
}

// --- Orchestration: BleMqttHandoffGate + DevMqttSmokeState composed as -
// --- ble_mqtt_main.cpp actually drives them, proving the properties     -
// --- requested for Phase 3C.4's hand-over: MQTT never starts before     -
// --- Wi-Fi/NTP are ready, a successful BLE-driven Wi-Fi connection      -
// --- alone starts the cascade, a credential failure never starts MQTT,  -
// --- and Wi-Fi already saved (no BLE at all this boot) reaches MQTT.    -

// While BLE still owns Wi-Fi (gate.bleOwnsWifi()==true, mirroring "no
// ARDUINO_EVENT_PROV_END observed yet"), the composed main.cpp never acts
// on a ConnectWifi action (never calls the real WiFi.begin(), never
// resolves the attempt via wifiAssociationStarted()/wifiAssociationResult())
// - so DevMqttSmokeState must never advance past WaitingForWifi, and MQTT
// must never be attempted, purely from its own unresolved in-flight
// bookkeeping, with zero orchestration-side action taken.
void test_gate_active_ble_ownership_blocks_connectivitys_own_wifi_action() {
    DevMqttSmokeState state(10, 40);
    BleMqttHandoffGate gate;

    const DevSmokeAction first = state.update(0, false, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi), static_cast<int>(first));
    TEST_ASSERT_FALSE(gate.shouldHandleConnectWifiAction());
    // Orchestration deliberately does nothing here (see ble_mqtt_main.cpp's
    // gated ConnectWifi branch) - no wifiAssociationStarted()/Result() call.

    // Still "in flight" from DevMqttSmokeState's own perspective (its
    // provisional timeout has not elapsed), so it must not reissue
    // ConnectWifi or advance state on its own, no matter how many more
    // ticks pass without wifiConnected ever becoming true.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(1, false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(5, false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
}

// A credential rejection (ARDUINO_EVENT_PROV_CRED_FAIL) never produces a
// real ARDUINO_EVENT_WIFI_STA_GOT_IP - wifiConnected stays false for as
// long as no corrected credential is accepted. MQTT (or even DNS/NTP) must
// never be attempted purely because time passes.
void test_credential_failure_never_reaches_dns_time_or_mqtt() {
    DevMqttSmokeState state(10, 40);
    for (uint32_t tick = 0; tick < 5; ++tick) {
        const DevSmokeAction action = state.update(tick * 20000, false, false, false);
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(action));
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime), static_cast<int>(action));
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(action));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
}

// A successful BLE credential application (ARDUINO_EVENT_PROV_CRED_SUCCESS
// followed by the real ARDUINO_EVENT_WIFI_STA_GOT_IP) starts the ordinary
// cascade purely from wifiConnected becoming true - even while
// gate.bleOwnsWifi() is still true (ARDUINO_EVENT_PROV_END, the manager's
// own auto-stop, has not fired yet). The gate only ever concerns whether
// *this environment's own* WiFi.begin() call is authorized, never whether
// an already-connected Wi-Fi interface may be used for DNS/NTP/MQTT.
void test_successful_provisioning_starts_the_sequence_even_before_hand_off() {
    DevMqttSmokeState state(10, 40);
    BleMqttHandoffGate gate;

    state.update(0, false, false, false); // ConnectWifi issued, gated, unacted - as above.
    TEST_ASSERT_TRUE(gate.bleOwnsWifi());

    // The official manager applies the credentials and connects Wi-Fi
    // itself; the DEV entry point only ever forwards the real GOT_IP event.
    state.wifiAssociationResult(1, true); // confirmed-connect window opens
    const DevSmokeAction afterConnect = state.update(2, true, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(afterConnect));
    TEST_ASSERT_TRUE(gate.bleOwnsWifi()); // PROV_END has not fired yet - still true.

    state.dnsResult(2, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime), static_cast<int>(state.update(2, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(3, true, true, false)));
    state.mqttResult(3, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

// Wi-Fi already saved from an earlier boot (the "already provisioned"
// case): no BLE session ever opens (ARDUINO_EVENT_PROV_START never fires),
// so this environment's local BleOnboardingWindow bookkeeping observes
// StartNotConfirmed and hands off immediately - but the manager may still
// be reconnecting directly in the background, and that reconnection alone
// (a real GOT_IP) must be enough to reach MQTT, exactly like a fresh BLE
// credential success.
void test_already_saved_wifi_reaches_mqtt_after_local_hand_off() {
    DevMqttSmokeState state(10, 40);
    BleMqttHandoffGate gate;

    state.update(0, false, false, false); // ConnectWifi issued, gated, unacted.
    gate.markProvisioningEnded(); // StartNotConfirmed observed locally - see ble_mqtt_main.cpp.
    TEST_ASSERT_TRUE(gate.shouldHandleConnectWifiAction());

    // The manager's own background reconnect (using the previously stored
    // config) finishes independently of this environment ever calling
    // WiFi.begin() itself.
    state.wifiAssociationResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(2, true, false, false)));
    state.dnsResult(2, true);
    state.update(2, true, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(3, true, true, false)));
    state.mqttResult(3, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

// After hand-off, a later Wi-Fi drop (a genuine steady-state hiccup, long
// after onboarding concluded) is this environment's own connectivity
// cascade to recover - the gate must authorize its own ConnectWifi action
// this time.
void test_hand_off_then_later_drop_authorizes_connectivitys_own_reconnect() {
    DevMqttSmokeState state(10, 40);
    BleMqttHandoffGate gate;

    state.update(0, false, false, false);
    state.wifiAssociationResult(1, true);
    state.update(2, true, false, false);
    state.dnsResult(2, true);
    state.update(2, true, false, false);
    state.update(3, true, true, false);
    state.mqttResult(3, true);
    gate.markProvisioningEnded();
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));

    // A real, unrelated Wi-Fi drop - a genuinely fresh loss gets a prompt
    // retry with no backoff delay (see DevMqttSmokeState::update()'s doc
    // comment), so the very first update() call after the drop already
    // reissues ConnectWifi.
    const DevSmokeAction reconnect = state.update(100, false, true, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi), static_cast<int>(reconnect));
    TEST_ASSERT_TRUE(gate.shouldHandleConnectWifiAction());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_gate_starts_with_ble_owning_wifi);
    RUN_TEST(test_gate_hands_off_once_provisioning_ends);
    RUN_TEST(test_gate_hand_off_is_a_one_shot_latch);
    RUN_TEST(test_gate_active_ble_ownership_blocks_connectivitys_own_wifi_action);
    RUN_TEST(test_credential_failure_never_reaches_dns_time_or_mqtt);
    RUN_TEST(test_successful_provisioning_starts_the_sequence_even_before_hand_off);
    RUN_TEST(test_already_saved_wifi_reaches_mqtt_after_local_hand_off);
    RUN_TEST(test_hand_off_then_later_drop_authorizes_connectivitys_own_reconnect);
    return UNITY_END();
}
