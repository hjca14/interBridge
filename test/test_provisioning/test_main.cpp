#include <unity.h>

#include "../../src/hardware/status_indicator.h"
#include "../../src/network/wifi.h"
#include "../../src/provisioning/ble_provisioning.h"
#include "../../src/provisioning/fleet_provisioning.h"
#include "../../src/provisioning/provisioning_manager.h"
#include "../../src/storage/credential_store.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

namespace {

constexpr const char* kTestDeviceId = "ib-0123456789abcdef0123456789abcdef";

// Bundles every collaborator ProvisioningManager needs, wired together
// with fakes, so each test starts from a fresh, independent instance.
struct Fixture {
    MemoryStore store;
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    DeviceCredentialStore credentials{store};
    FakeKeyPairGenerator keyGen;
    FakeFleetProvisioningTransport fleetTransport;
    FleetProvisioningCoordinator fleetProvisioning{keyGen, fleetTransport, credentials, "TestTemplate"};
    FakeStatusIndicator statusIndicator;
    ProvisioningManager manager{
        store, wifi, ble, credentials, fleetProvisioning, statusIndicator, kTestDeviceId, "pop-123",
        BleAdvertisementInfo{"InterBridge-TEST", "TEST", true}};
};

} // namespace

void setUp() {}
void tearDown() {}

void test_check_at_boot_enters_provisioning_when_no_credentials() {
    Fixture f;
    f.manager.checkAtBoot(0);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());
    TEST_ASSERT_EQUAL_STRING("pop-123", f.ble.lastProofOfPossession().c_str());
    TEST_ASSERT_EQUAL_STRING("InterBridge-TEST", f.ble.lastAdvertisementInfo().deviceName.c_str());

    auto event = f.manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningStarted), static_cast<int>(*event));
}

void test_check_at_boot_stays_idle_when_credentials_already_stored() {
    Fixture f;
    f.store.set("wifi_ssid", "MyNetwork");

    f.manager.checkAtBoot(0);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Idle), static_cast<int>(f.manager.state()));
    TEST_ASSERT_FALSE(f.ble.isAdvertising());
}

void test_ble_session_active_transitions_and_stops_advertising() {
    Fixture f;
    f.manager.checkAtBoot(0);

    f.ble.setSessionActive(true);
    f.manager.update(100);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::BleSessionActive), static_cast<int>(f.manager.state()));
    TEST_ASSERT_FALSE(f.ble.isAdvertising());
}

void test_session_drop_before_credentials_resumes_advertising() {
    Fixture f;
    f.manager.checkAtBoot(0);
    f.ble.setSessionActive(true);
    f.manager.update(100);
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::BleSessionActive), static_cast<int>(f.manager.state()));

    f.ble.setSessionActive(false);
    f.manager.update(200);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());
}

void test_receiving_credentials_starts_wifi_connection() {
    Fixture f;
    f.manager.checkAtBoot(0);

    f.ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    f.manager.update(100);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ConnectingWifi), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.wifi.beginCalled());
    TEST_ASSERT_EQUAL_STRING("MyNetwork", f.wifi.lastSsid().c_str());
    TEST_ASSERT_FALSE(f.ble.isAdvertising());
    TEST_ASSERT_TRUE(f.store.get("wifi_ssid").has_value());
}

void test_full_flow_runs_fleet_provisioning_and_completes() {
    Fixture f;
    f.manager.checkAtBoot(0);
    f.ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    f.manager.update(100); // -> ConnectingWifi

    f.wifi.setConnected(true);
    f.manager.update(200); // -> FleetProvisioning -> CloudConnecting -> Provisioned

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Provisioned), static_cast<int>(f.manager.state()));
    TEST_ASSERT_EQUAL(1, f.fleetTransport.createCertificateCalls());
    TEST_ASSERT_TRUE(f.credentials.hasCertificate());
    TEST_ASSERT_EQUAL_STRING("1", f.store.get("provisioned")->c_str());

    auto event = f.manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningCompleted), static_cast<int>(*event));

    TEST_ASSERT_TRUE(f.statusIndicator.hasIndication());
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningIndication::ProvisioningSuccess),
                       static_cast<int>(f.statusIndicator.lastIndication()));
}

void test_existing_certificate_skips_fleet_provisioning() {
    Fixture f;
    f.credentials.saveCertificate("already-have-a-cert");
    f.credentials.savePrivateKey("already-have-a-key");

    f.manager.checkAtBoot(0);
    f.ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    f.manager.update(100);
    f.wifi.setConnected(true);
    f.manager.update(200);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Provisioned), static_cast<int>(f.manager.state()));
    TEST_ASSERT_EQUAL(0, f.fleetTransport.createCertificateCalls());
}

void test_fleet_provisioning_failure_retries_within_window() {
    Fixture f;
    f.fleetTransport.setRegisterThingResult(false);

    f.manager.checkAtBoot(0);
    f.ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    f.manager.update(100);
    f.wifi.setConnected(true);
    f.manager.update(200); // Fleet Provisioning fails, should retry (still well within the 5-minute window)

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());

    auto event = f.manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningFailed), static_cast<int>(*event));

    // A second attempt (e.g. the app retries) can still succeed.
    f.fleetTransport.setRegisterThingResult(true);
    f.ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    f.manager.update(300);
    f.wifi.setConnected(true);
    f.manager.update(400);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Provisioned), static_cast<int>(f.manager.state()));
}

void test_provisioning_window_expires_and_returns_to_not_provisioned() {
    Fixture f;
    f.manager.checkAtBoot(0);

    f.manager.update(kProvisioningWindowMs - 1); // still within the window
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());

    f.manager.update(kProvisioningWindowMs); // window elapsed

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::NotProvisioned), static_cast<int>(f.manager.state()));
    TEST_ASSERT_FALSE(f.ble.isAdvertising());

    auto event = f.manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningFailed), static_cast<int>(*event));
}

void test_provisioning_window_expiry_on_already_provisioned_device_returns_to_idle() {
    Fixture f;
    f.store.set("wifi_ssid", "MyNetwork"); // already configured
    f.manager.requestProvisioning(0);       // re-opened via the physical button

    f.manager.update(kProvisioningWindowMs);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Idle), static_cast<int>(f.manager.state()));
}

void test_request_provisioning_is_ignored_while_already_in_progress() {
    Fixture f;
    f.manager.requestProvisioning(0);
    f.manager.pollEvent(); // clear the first ProvisioningStarted event

    f.manager.requestProvisioning(50); // should be a no-op

    auto event = f.manager.pollEvent();
    TEST_ASSERT_FALSE(event.has_value());
}

void test_configured_device_reopens_provisioning_on_button_request() {
    Fixture f;
    f.store.set("wifi_ssid", "MyNetwork");
    f.manager.checkAtBoot(0);
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Idle), static_cast<int>(f.manager.state()));

    f.manager.requestProvisioning(1000);

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable), static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());
}

void test_ble_start_failure_is_observable_and_can_be_retried() {
    Fixture f;
    f.ble.setStartResult(false);
    f.manager.checkAtBoot(0);
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningFailed),
                      static_cast<int>(f.manager.state()));
    auto event = f.manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningFailed), static_cast<int>(*event));

    f.ble.setStartResult(true);
    f.manager.requestProvisioning(100);
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ProvisioningAvailable),
                      static_cast<int>(f.manager.state()));
    TEST_ASSERT_TRUE(f.ble.isAdvertising());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_check_at_boot_enters_provisioning_when_no_credentials);
    RUN_TEST(test_check_at_boot_stays_idle_when_credentials_already_stored);
    RUN_TEST(test_ble_session_active_transitions_and_stops_advertising);
    RUN_TEST(test_session_drop_before_credentials_resumes_advertising);
    RUN_TEST(test_receiving_credentials_starts_wifi_connection);
    RUN_TEST(test_full_flow_runs_fleet_provisioning_and_completes);
    RUN_TEST(test_existing_certificate_skips_fleet_provisioning);
    RUN_TEST(test_fleet_provisioning_failure_retries_within_window);
    RUN_TEST(test_provisioning_window_expires_and_returns_to_not_provisioned);
    RUN_TEST(test_provisioning_window_expiry_on_already_provisioned_device_returns_to_idle);
    RUN_TEST(test_request_provisioning_is_ignored_while_already_in_progress);
    RUN_TEST(test_configured_device_reopens_provisioning_on_button_request);
    RUN_TEST(test_ble_start_failure_is_observable_and_can_be_retried);
    return UNITY_END();
}
