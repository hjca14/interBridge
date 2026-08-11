#include <unity.h>

#include "../../src/network/wifi.h"
#include "../../src/provisioning/ble_provisioning.h"
#include "../../src/provisioning/provisioning_manager.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_check_at_boot_enters_provisioning_when_no_credentials() {
    MemoryStore store;
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    ProvisioningManager manager(store, wifi, ble, "pop-123");

    manager.checkAtBoot();

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::AwaitingCredentials), static_cast<int>(manager.state()));
    TEST_ASSERT_TRUE(ble.isAdvertising());
    TEST_ASSERT_EQUAL_STRING("pop-123", ble.lastProofOfPossession().c_str());

    auto event = manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningStarted), static_cast<int>(*event));
}

void test_check_at_boot_stays_idle_when_credentials_already_stored() {
    MemoryStore store;
    store.set("wifi_ssid", "MyNetwork");
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    ProvisioningManager manager(store, wifi, ble, "pop-123");

    manager.checkAtBoot();

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Idle), static_cast<int>(manager.state()));
    TEST_ASSERT_FALSE(ble.isAdvertising());
}

void test_receiving_credentials_starts_wifi_connection() {
    MemoryStore store;
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    ProvisioningManager manager(store, wifi, ble, "pop-123");

    manager.checkAtBoot();
    ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    manager.update();

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::ConnectingWifi), static_cast<int>(manager.state()));
    TEST_ASSERT_TRUE(wifi.beginCalled());
    TEST_ASSERT_EQUAL_STRING("MyNetwork", wifi.lastSsid().c_str());
    TEST_ASSERT_FALSE(ble.isAdvertising());
    TEST_ASSERT_TRUE(store.get("wifi_ssid").has_value());
}

void test_wifi_connecting_then_completes_provisioning() {
    MemoryStore store;
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    ProvisioningManager manager(store, wifi, ble, "pop-123");

    manager.checkAtBoot();
    ble.injectCredentials(WifiCredentialsPayload{"MyNetwork", "hunter2"});
    manager.update(); // -> ConnectingWifi

    wifi.setConnected(true);
    manager.update(); // -> Completed

    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningState::Completed), static_cast<int>(manager.state()));
    auto event = manager.pollEvent();
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolEventName::ProvisioningCompleted), static_cast<int>(*event));
}

void test_request_provisioning_is_ignored_while_already_in_progress() {
    MemoryStore store;
    FakeWifiConnection wifi;
    FakeBleProvisioning ble;
    ProvisioningManager manager(store, wifi, ble, "pop-123");

    manager.requestProvisioning();
    manager.pollEvent(); // clear the first ProvisioningStarted event

    manager.requestProvisioning(); // should be a no-op

    auto event = manager.pollEvent();
    TEST_ASSERT_FALSE(event.has_value());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_check_at_boot_enters_provisioning_when_no_credentials);
    RUN_TEST(test_check_at_boot_stays_idle_when_credentials_already_stored);
    RUN_TEST(test_receiving_credentials_starts_wifi_connection);
    RUN_TEST(test_wifi_connecting_then_completes_provisioning);
    RUN_TEST(test_request_provisioning_is_ignored_while_already_in_progress);
    return UNITY_END();
}
