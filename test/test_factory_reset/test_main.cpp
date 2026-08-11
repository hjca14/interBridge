#include <unity.h>

#include "../../src/provisioning/factory_reset_coordinator.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_factory_reset_clears_wifi_credentials() {
    MemoryStore store;
    store.set("wifi_ssid", "MyNetwork");
    store.set("wifi_password", "hunter2");
    store.set("provisioned", "1");

    FactoryResetCoordinator coordinator(store);
    bool result = coordinator.execute();

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(store.has("wifi_ssid"));
    TEST_ASSERT_FALSE(store.has("wifi_password"));
    TEST_ASSERT_EQUAL_STRING("0", store.get("provisioned")->c_str());
}

void test_factory_reset_preserves_device_identity_and_credentials() {
    MemoryStore store;
    store.set("device_id", "ib-0123456789abcdef0123456789abcdef");
    store.set("aws_certificate_pem", "-----BEGIN CERTIFICATE-----...");
    store.set("aws_private_key_pem", "-----BEGIN PRIVATE KEY-----...");
    store.set("wifi_ssid", "MyNetwork");

    FactoryResetCoordinator coordinator(store);
    coordinator.execute();

    TEST_ASSERT_TRUE(store.has("device_id"));
    TEST_ASSERT_TRUE(store.has("aws_certificate_pem"));
    TEST_ASSERT_TRUE(store.has("aws_private_key_pem"));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_factory_reset_clears_wifi_credentials);
    RUN_TEST(test_factory_reset_preserves_device_identity_and_credentials);
    return UNITY_END();
}
