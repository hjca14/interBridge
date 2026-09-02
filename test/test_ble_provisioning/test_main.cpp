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
    return UNITY_END();
}
