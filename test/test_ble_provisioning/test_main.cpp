#include <unity.h>

#include "../../src/provisioning/ble_provisioning.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_advertisement_info_uses_last_four_hex_chars_uppercased() {
    BleAdvertisementInfo info = buildBleAdvertisementInfo("ib-0123456789abcdef0123456789abc0a91c");
    TEST_ASSERT_EQUAL_STRING("A91C", info.deviceIdentityHint.c_str());
    TEST_ASSERT_EQUAL_STRING("InterBridge-A91C", info.deviceName.c_str());
    TEST_ASSERT_TRUE(info.provisioningAvailable);
}

void test_advertisement_info_never_contains_secrets() {
    // Sanity: nothing about a setup_code, PoP, or credential is part of
    // the advertisement model - it is built from device_id alone.
    BleAdvertisementInfo info = buildBleAdvertisementInfo("ib-0123456789abcdef0123456789abcdef");
    TEST_ASSERT_TRUE(info.deviceName.find("482719362051") == std::string::npos);
}

void test_default_security_mode_is_never_plaintext() {
    // BleSecurityMode has no plaintext/"None" value by construction - this
    // test exists to make that guarantee explicit and catch a regression
    // if the enum is ever extended carelessly.
    FakeBleProvisioning ble;
    TEST_ASSERT_TRUE(ble.securityMode() == BleSecurityMode::Security1 ||
                      ble.securityMode() == BleSecurityMode::Security2);
}

void test_security_mode_is_configurable() {
    FakeBleProvisioning ble(BleSecurityMode::Security1);
    TEST_ASSERT_EQUAL(static_cast<int>(BleSecurityMode::Security1), static_cast<int>(ble.securityMode()));
}

void test_start_advertising_records_info_and_pop() {
    FakeBleProvisioning ble;
    BleAdvertisementInfo info{"InterBridge-TEST", "TEST", true};

    TEST_ASSERT_TRUE(ble.startAdvertising(info, "pop-abc"));
    TEST_ASSERT_TRUE(ble.isAdvertising());
    TEST_ASSERT_EQUAL_STRING("pop-abc", ble.lastProofOfPossession().c_str());
    TEST_ASSERT_EQUAL_STRING("InterBridge-TEST", ble.lastAdvertisementInfo().deviceName.c_str());
}

void test_session_active_defaults_false_and_is_settable() {
    FakeBleProvisioning ble;
    TEST_ASSERT_FALSE(ble.isSessionActive());
    ble.setSessionActive(true);
    TEST_ASSERT_TRUE(ble.isSessionActive());
}

void test_real_transport_lifecycle_metadata_never_exposes_credentials() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    ble.notifySecureSessionEstablished();
    TEST_ASSERT_TRUE(ble.isSessionActive());
    ble.notifyCredentials("private-ssid", "private-password");
    auto credentials = ble.pollReceivedCredentials();
    TEST_ASSERT_TRUE(credentials.has_value());
    TEST_ASSERT_EQUAL_STRING("private-ssid", credentials->ssid.c_str());
    TEST_ASSERT_FALSE(ble.pollReceivedCredentials().has_value());
    ble.notifyDisconnected();
    TEST_ASSERT_FALSE(ble.isSessionActive());
}

void test_fake_can_model_sanitized_start_failure_and_retry() {
    FakeBleProvisioning ble(BleSecurityMode::Security1);
    BleAdvertisementInfo info{"InterBridge-A91C", "A91C", true};
    ble.setStartResult(false);
    TEST_ASSERT_FALSE(ble.startAdvertising(info, "secret-pop"));
    TEST_ASSERT_FALSE(ble.isAdvertising());
    ble.setStartResult(true);
    TEST_ASSERT_TRUE(ble.startAdvertising(info, "secret-pop"));
    TEST_ASSERT_TRUE(ble.isAdvertising());
}

// Real bench observation: a physical run printed "onboarding window
// closed: timeout" while nRF Connect never saw an InterBridge-XXXX
// device - the WiFiProv start request had been wrongly treated as proof
// advertising was active. These tests prove the fix: a start request
// alone never confirms advertising; only a real ARDUINO_EVENT_PROV_START
// (via notifyAdvertisingStarted()/BleOnboardingWindow::confirmStart())
// does, and a missing confirmation fails closed rather than opening a
// window for a service that never started.

void test_real_transport_start_request_alone_does_not_confirm_advertising() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    TEST_ASSERT_FALSE(ble.isAdvertising());

    BleOnboardingWindow window(10000, 300000);
    window.requestStart(0);
    TEST_ASSERT_TRUE(window.isAwaitingConfirmation());
    TEST_ASSERT_FALSE(window.isOpen());
    TEST_ASSERT_FALSE(ble.isAdvertising());
}

void test_prov_start_confirmation_activates_advertising_and_window() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    BleOnboardingWindow window(10000, 300000);

    window.requestStart(0);
    ble.notifyAdvertisingStarted();
    window.confirmStart(50);

    TEST_ASSERT_TRUE(ble.isAdvertising());
    TEST_ASSERT_TRUE(window.isOpen());
    TEST_ASSERT_FALSE(window.isAwaitingConfirmation());

    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(50 + 299999)));
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::WindowTimedOut),
                       static_cast<int>(window.update(50 + 300000)));
    TEST_ASSERT_FALSE(window.isOpen());
}

void test_missing_start_confirmation_fails_closed() {
    BleOnboardingWindow window(10000, 300000);
    window.requestStart(0);

    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(9999)));
    TEST_ASSERT_TRUE(window.isAwaitingConfirmation());

    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::StartNotConfirmed),
                       static_cast<int>(window.update(10000)));
    TEST_ASSERT_FALSE(window.isAwaitingConfirmation());
    TEST_ASSERT_FALSE(window.isOpen());

    // A confirmation arriving after the deadline was already missed must
    // not resurrect this attempt.
    window.confirmStart(10001);
    TEST_ASSERT_FALSE(window.isOpen());

    // The failure must not repeat on a later tick.
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(20000)));
}

void test_retry_remains_possible_after_a_failed_start() {
    BleOnboardingWindow window(10000, 300000);
    window.requestStart(0);
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::StartNotConfirmed),
                       static_cast<int>(window.update(10000)));

    // A fresh retry (e.g. after Esp32BleProvisioning::stopAdvertising()
    // followed by a new startAdvertising() call) opens a full new window.
    window.requestStart(20000);
    TEST_ASSERT_TRUE(window.isAwaitingConfirmation());
    window.confirmStart(20050);
    TEST_ASSERT_TRUE(window.isOpen());
}

// A second bench run showed the provisioning manager initializing
// (ARDUINO_EVENT_PROV_INIT) but never confirming a start - the isolated
// DEV entry point was then changed to call requestStart() BEFORE
// startAdvertising(), because ARDUINO_EVENT_PROV_START can be dispatched
// (on the system event task) essentially immediately once
// WiFiProv.beginProvision() is called, possibly before control even
// returns to the caller. These tests prove that ordering fix at the
// BleOnboardingWindow level: a confirmation landing on the very same
// tick the window was armed still opens it, and a locally rejected
// request (startAdvertising() returning false) leaves no window armed.

void test_confirm_start_on_the_same_tick_as_request_still_opens_window() {
    // Models ARDUINO_EVENT_PROV_START firing synchronously from within
    // the same call that armed requestStart() - confirmStart() must not
    // be lost as a no-op just because it landed on the very same tick.
    BleOnboardingWindow window(10000, 300000);
    window.requestStart(1000);
    window.confirmStart(1000);
    TEST_ASSERT_TRUE(window.isOpen());
    TEST_ASSERT_FALSE(window.isAwaitingConfirmation());
}

void test_close_after_locally_rejected_request_leaves_no_window_armed() {
    // Models startAdvertising() returning false (local validation
    // rejected the request, e.g. empty PoP/device name) right after
    // requestStart() armed the window - close() must leave nothing armed
    // for a request that never actually went out to WiFiProv.
    BleOnboardingWindow window(10000, 300000);
    window.requestStart(1000);
    window.close();
    TEST_ASSERT_FALSE(window.isAwaitingConfirmation());
    TEST_ASSERT_FALSE(window.isOpen());
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(50000)));

    // A confirmation that still arrives late (e.g. a delayed event from
    // the rejected attempt) must not resurrect it either.
    window.confirmStart(50000);
    TEST_ASSERT_FALSE(window.isOpen());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_advertisement_info_uses_last_four_hex_chars_uppercased);
    RUN_TEST(test_advertisement_info_never_contains_secrets);
    RUN_TEST(test_default_security_mode_is_never_plaintext);
    RUN_TEST(test_security_mode_is_configurable);
    RUN_TEST(test_start_advertising_records_info_and_pop);
    RUN_TEST(test_session_active_defaults_false_and_is_settable);
    RUN_TEST(test_real_transport_lifecycle_metadata_never_exposes_credentials);
    RUN_TEST(test_fake_can_model_sanitized_start_failure_and_retry);
    RUN_TEST(test_real_transport_start_request_alone_does_not_confirm_advertising);
    RUN_TEST(test_prov_start_confirmation_activates_advertising_and_window);
    RUN_TEST(test_missing_start_confirmation_fails_closed);
    RUN_TEST(test_retry_remains_possible_after_a_failed_start);
    RUN_TEST(test_confirm_start_on_the_same_tick_as_request_still_opens_window);
    RUN_TEST(test_close_after_locally_rejected_request_leaves_no_window_armed);
    return UNITY_END();
}
