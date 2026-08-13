#include <unity.h>

#include "../../src/provisioning/device_identity.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_valid_device_id_format() {
    TEST_ASSERT_TRUE(isValidDeviceId("ib-0123456789abcdef0123456789abcdef"));
}

void test_invalid_device_id_wrong_prefix() {
    TEST_ASSERT_FALSE(isValidDeviceId("xx-0123456789abcdef0123456789abcdef"));
}

void test_invalid_device_id_wrong_length() {
    TEST_ASSERT_FALSE(isValidDeviceId("ib-0123"));
}

void test_invalid_device_id_uppercase_hex() {
    TEST_ASSERT_FALSE(isValidDeviceId("ib-0123456789ABCDEF0123456789abcdef"));
}

void test_valid_setup_code_format() {
    TEST_ASSERT_TRUE(isValidSetupCode("482719362051"));
}

void test_invalid_setup_code_wrong_length() {
    TEST_ASSERT_FALSE(isValidSetupCode("12345"));
}

void test_invalid_setup_code_non_digit() {
    TEST_ASSERT_FALSE(isValidSetupCode("48271936205a"));
}

void test_load_generates_valid_id_on_first_boot() {
    MemoryStore store;
    FakeRandomSource random(1);
    DeviceIdentityProvider provider(store, random, "1.0", "0.1.0");

    DeviceIdentity identity = provider.load();

    TEST_ASSERT_TRUE(isValidDeviceId(identity.deviceId));
    TEST_ASSERT_TRUE(isValidSetupCode(identity.setupCode));
    TEST_ASSERT_EQUAL_STRING("1.0", identity.hardwareVersion.c_str());
    TEST_ASSERT_EQUAL_STRING("0.1.0", identity.firmwareVersion.c_str());
    TEST_ASSERT_FALSE(identity.provisioned);
}

void test_load_is_stable_across_reboots() {
    MemoryStore store;
    FakeRandomSource random(1);
    DeviceIdentityProvider provider(store, random, "1.0", "0.1.0");

    DeviceIdentity first = provider.load();

    DeviceIdentityProvider providerAfterReboot(store, random, "1.0", "0.1.1");
    DeviceIdentity second = providerAfterReboot.load();

    TEST_ASSERT_EQUAL_STRING(first.deviceId.c_str(), second.deviceId.c_str());
    TEST_ASSERT_EQUAL_STRING(first.setupCode.c_str(), second.setupCode.c_str());
    // Firmware version reflects the current boot's build, not a stored value.
    TEST_ASSERT_EQUAL_STRING("0.1.1", second.firmwareVersion.c_str());
}

void test_set_provisioned_persists() {
    MemoryStore store;
    FakeRandomSource random(1);
    DeviceIdentityProvider provider(store, random, "1.0", "0.1.0");
    provider.load();
    provider.setProvisioned(true);

    DeviceIdentityProvider reloaded(store, random, "1.0", "0.1.0");
    TEST_ASSERT_TRUE(reloaded.load().provisioned);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_valid_device_id_format);
    RUN_TEST(test_invalid_device_id_wrong_prefix);
    RUN_TEST(test_invalid_device_id_wrong_length);
    RUN_TEST(test_invalid_device_id_uppercase_hex);
    RUN_TEST(test_valid_setup_code_format);
    RUN_TEST(test_invalid_setup_code_wrong_length);
    RUN_TEST(test_invalid_setup_code_non_digit);
    RUN_TEST(test_load_generates_valid_id_on_first_boot);
    RUN_TEST(test_load_is_stable_across_reboots);
    RUN_TEST(test_set_provisioned_persists);
    return UNITY_END();
}
