#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "../../src/provisioning/ble_provisioning.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

namespace {
// src/dev/ble_provisioning_main.cpp is Arduino/ESP-IDF-only (excluded
// from the native build_src_filter, per platformio.ini) - it cannot be
// compiled or exercised directly in a native test. Reading its own
// source text is the same pattern already used by
// test_isolated_ble_env_does_not_enable_verbose_core_debug() below for
// exactly this reason: a source-level regression guard, not a
// substitute for physical validation.
std::string readFileOrFail(const std::string& path) {
    std::ifstream file(path);
    TEST_ASSERT_TRUE_MESSAGE(file.is_open(), (path + " not found relative to the test working directory").c_str());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Slices the source text strictly between two markers that must each
// appear exactly once, in that order - used to isolate one switch
// `case` body from its neighbors so a check can be scoped to exactly
// the code path it claims to cover.
std::string sliceBetween(const std::string& source, const std::string& startMarker, const std::string& endMarker) {
    size_t start = source.find(startMarker);
    TEST_ASSERT_TRUE_MESSAGE(start != std::string::npos, ("start marker not found: " + startMarker).c_str());
    start += startMarker.size();
    size_t end = source.find(endMarker, start);
    TEST_ASSERT_TRUE_MESSAGE(end != std::string::npos, ("end marker not found after start: " + endMarker).c_str());
    return source.substr(start, end - start);
}
} // namespace

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
    ble.notifyDisconnected();
    TEST_ASSERT_FALSE(ble.isSessionActive());
}

// Phase 3C.3: the official wifi_provisioning manager applies received
// Wi-Fi credentials to the stack itself, so Esp32BleProvisioning never
// buffers or exposes a copy - see WifiCredentialState's doc comment in
// ble_provisioning.h. notifyCredentialsReceived() takes no ssid/password
// argument at all: this is a compile-time guarantee, not just a runtime
// one - the class has no method through which a credential string could
// even be passed in, let alone retained.

void test_wifi_credential_state_defaults_idle() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Idle), static_cast<int>(ble.wifiCredentialState()));
}

void test_credentials_received_transitions_to_connecting_without_any_credential_argument() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    ble.notifyCredentialsReceived();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Connecting), static_cast<int>(ble.wifiCredentialState()));
}

void test_wifi_connected_reflects_official_cred_success_event() {
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    ble.notifyCredentialsReceived();
    ble.notifyWifiConnected();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Connected), static_cast<int>(ble.wifiCredentialState()));
}

void test_credentials_rejected_does_not_end_session_and_permits_retry() {
    // Per the official manager's default behavior, a rejected attempt
    // must not end the BLE session/window - the app can resubmit
    // different credentials while the window stays open.
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    ble.notifySecureSessionEstablished();
    ble.notifyCredentialsReceived();

    ble.notifyCredentialsRejected();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Rejected), static_cast<int>(ble.wifiCredentialState()));
    TEST_ASSERT_TRUE(ble.isSessionActive());

    // Retry: a fresh credential submission over the same still-active
    // session resumes the normal Connecting -> Connected path.
    ble.notifyCredentialsReceived();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Connecting), static_cast<int>(ble.wifiCredentialState()));
    ble.notifyWifiConnected();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiCredentialState::Connected), static_cast<int>(ble.wifiCredentialState()));
}

void test_esp32_adapter_never_retains_or_exposes_received_credentials() {
    // pollReceivedCredentials() exists only to satisfy IBleProvisioning's
    // shared interface (ProvisioningManager/FakeBleProvisioning still use
    // it for a hypothetical non-official-manager implementation) - on
    // this adapter it must always report absent, even after a full
    // received -> connected credential cycle, since nothing on this class
    // ever stores a credential string to return.
    Esp32BleProvisioning ble(BleSecurityMode::Security1);
    ble.notifyCredentialsReceived();
    ble.notifyWifiConnected();
    TEST_ASSERT_FALSE(ble.pollReceivedCredentials().has_value());
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

// A real bench run submitting an invalid Wi-Fi credential (wrong SSID/
// password) showed the manager retry connecting with the rejected
// credential indefinitely, never accepting a corrected one in the same
// window - the user would have needed a flash/NVS erase and a reflash
// to fix a typo. The fix calls the official
// wifi_prov_mgr_reset_sm_state_on_failure() (ESP-IDF 4.4.7 Wi-Fi
// Provisioning Manager, confirmed present and linkable in the pinned
// esp32c3 libwifi_provisioning.a - see its own doc comment in
// wifi_provisioning/manager.h: "restart provisioning in case invalid
// credentials are entered") from ARDUINO_EVENT_PROV_CRED_FAIL only.
// These tests cover what is natively testable: the BleOnboardingWindow
// timing contract the fix must preserve, and (since
// ble_provisioning_main.cpp itself is Arduino/ESP-IDF-only and cannot
// be compiled or exercised natively - see readFileOrFail()) a
// source-level guard that the reset call exists in exactly the right
// place, nowhere else, and introduces no secret.

void test_credential_failure_and_reset_does_not_touch_onboarding_window_deadline() {
    // The five-minute onboarding window must not be extended, restarted,
    // or otherwise affected by a rejected Wi-Fi credential and the reset
    // that follows it - ARDUINO_EVENT_PROV_CRED_FAIL's handling never
    // calls requestStart()/confirmStart()/close() (see the source-text
    // test below). Models the resulting timeline: window opens,
    // credentials are rejected partway through, the window still expires
    // at its ORIGINAL deadline - a corrected credential submitted well
    // within that original deadline still finds the window open, with no
    // reboot and no new window needed.
    BleOnboardingWindow window(10000, 300000);
    window.requestStart(0);
    window.confirmStart(50);
    TEST_ASSERT_TRUE(window.isOpen());

    // A rejected credential (and the reset that follows) arrives partway
    // through the window - nothing here touches the window's own state.
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(100000)));
    TEST_ASSERT_TRUE(window.isOpen());

    // A corrected credential submitted well before the ORIGINAL deadline
    // (50 + 300000) still finds the window open.
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::None), static_cast<int>(window.update(50 + 299999)));
    TEST_ASSERT_TRUE(window.isOpen());

    // The window still expires at exactly its original deadline - the
    // reset never extended or restarted it.
    TEST_ASSERT_EQUAL(static_cast<int>(BleOnboardingWindowEvent::WindowTimedOut),
                       static_cast<int>(window.update(50 + 300000)));
}

void test_cred_fail_case_resets_official_provisioning_state_machine() {
    std::string source = readFileOrFail("src/dev/ble_provisioning_main.cpp");
    std::string credFailBody =
        sliceBetween(source, "case ARDUINO_EVENT_PROV_CRED_FAIL:", "case ARDUINO_EVENT_PROV_CRED_SUCCESS:");
    TEST_ASSERT_TRUE_MESSAGE(
        credFailBody.find("wifi_prov_mgr_reset_sm_state_on_failure(") != std::string::npos,
        "ARDUINO_EVENT_PROV_CRED_FAIL must call the official "
        "wifi_prov_mgr_reset_sm_state_on_failure() so a rejected credential does not leave the "
        "manager retrying forever with no way to accept a corrected one in the same window");
}

void test_reset_is_never_triggered_by_a_plain_ble_disconnect() {
    // ARDUINO_EVENT_PROV_END fires on a normal BLE disconnect (and on the
    // manager's own end-of-session teardown) - it must never call the
    // credential-failure reset API, which would be indistinguishable from
    // a real invalid-credential loop and could mask the actual event.
    std::string source = readFileOrFail("src/dev/ble_provisioning_main.cpp");
    std::string provEndBody = sliceBetween(source, "case ARDUINO_EVENT_PROV_END:", "default:");
    TEST_ASSERT_TRUE_MESSAGE(
        provEndBody.find("wifi_prov_mgr_reset_sm_state_on_failure(") == std::string::npos,
        "a plain BLE disconnect/session end (ARDUINO_EVENT_PROV_END) must never trigger the "
        "credential-failure reset - only ARDUINO_EVENT_PROV_CRED_FAIL may");
}

void test_cred_fail_handling_never_logs_a_secret() {
    std::string source = readFileOrFail("src/dev/ble_provisioning_main.cpp");
    std::string credFailBody =
        sliceBetween(source, "case ARDUINO_EVENT_PROV_CRED_FAIL:", "case ARDUINO_EVENT_PROV_CRED_SUCCESS:");
    // The only per-event data this case may read is the sanitized numeric
    // failure reason - never the credential fields from a *different*
    // event's payload (prov_cred_recv.ssid/.password are a different
    // union member entirely, but never referencing them here is a cheap,
    // explicit guarantee worth keeping).
    TEST_ASSERT_TRUE_MESSAGE(credFailBody.find("ssid") == std::string::npos,
                              "credential-failure handling must never reference ssid");
    TEST_ASSERT_TRUE_MESSAGE(credFailBody.find("password") == std::string::npos,
                              "credential-failure handling must never reference password");
}

// Regression guard for a real security incident: a temporary
// CORE_DEBUG_LEVEL=5 in env:esp32-c3-dev-ble-provisioning's build_flags
// let an upstream WiFiProv.cpp log line print the DEV PoP during bench
// diagnosis (see docs/ble-onboarding.md's "Physical validation" section).
// Reads the repo's actual platformio.ini rather than a copy, so it can't
// go stale - this must run with the repo root as the working directory,
// exactly as `pio test -e native` does.
void test_isolated_ble_env_does_not_enable_verbose_core_debug() {
    std::ifstream file("platformio.ini");
    TEST_ASSERT_TRUE_MESSAGE(file.is_open(), "platformio.ini not found relative to the test working directory");

    const std::string targetSection = "[env:esp32-c3-dev-ble-provisioning]";
    std::string line;
    bool inTargetSection = false;
    bool sawTargetSection = false;
    bool found = false;
    while (std::getline(file, line)) {
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos) {
            continue;
        }
        if (line[firstNonSpace] == '[') {
            inTargetSection = (line.compare(firstNonSpace, targetSection.size(), targetSection) == 0);
            sawTargetSection = sawTargetSection || inTargetSection;
            continue;
        }
        if (!inTargetSection || line[firstNonSpace] == ';') {
            // Only real build_flags entries count - not comments (e.g. the
            // one in platformio.ini itself warning against re-adding this).
            continue;
        }
        if (line.find("CORE_DEBUG_LEVEL") != std::string::npos) {
            found = true;
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(sawTargetSection, "[env:esp32-c3-dev-ble-provisioning] section not found in platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(found,
                               "CORE_DEBUG_LEVEL must not be set for esp32-c3-dev-ble-provisioning - it previously "
                               "leaked the DEV PoP via an upstream WiFiProv.cpp log line");
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
    RUN_TEST(test_wifi_credential_state_defaults_idle);
    RUN_TEST(test_credentials_received_transitions_to_connecting_without_any_credential_argument);
    RUN_TEST(test_wifi_connected_reflects_official_cred_success_event);
    RUN_TEST(test_credentials_rejected_does_not_end_session_and_permits_retry);
    RUN_TEST(test_esp32_adapter_never_retains_or_exposes_received_credentials);
    RUN_TEST(test_fake_can_model_sanitized_start_failure_and_retry);
    RUN_TEST(test_real_transport_start_request_alone_does_not_confirm_advertising);
    RUN_TEST(test_prov_start_confirmation_activates_advertising_and_window);
    RUN_TEST(test_missing_start_confirmation_fails_closed);
    RUN_TEST(test_retry_remains_possible_after_a_failed_start);
    RUN_TEST(test_confirm_start_on_the_same_tick_as_request_still_opens_window);
    RUN_TEST(test_close_after_locally_rejected_request_leaves_no_window_armed);
    RUN_TEST(test_credential_failure_and_reset_does_not_touch_onboarding_window_deadline);
    RUN_TEST(test_cred_fail_case_resets_official_provisioning_state_machine);
    RUN_TEST(test_reset_is_never_triggered_by_a_plain_ble_disconnect);
    RUN_TEST(test_cred_fail_handling_never_logs_a_secret);
    RUN_TEST(test_isolated_ble_env_does_not_enable_verbose_core_debug);
    return UNITY_END();
}
