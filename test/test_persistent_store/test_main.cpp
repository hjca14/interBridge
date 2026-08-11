#include <unity.h>

#include "../../src/storage/credential_store.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_memory_store_basic_get_set() {
    MemoryStore store;
    TEST_ASSERT_FALSE(store.has("key"));
    store.set("key", "value");
    TEST_ASSERT_TRUE(store.has("key"));
    TEST_ASSERT_EQUAL_STRING("value", store.get("key")->c_str());
}

void test_memory_store_remove() {
    MemoryStore store;
    store.set("key", "value");
    store.remove("key");
    TEST_ASSERT_FALSE(store.has("key"));
    TEST_ASSERT_FALSE(store.get("key").has_value());
}

void test_memory_store_overwrite() {
    MemoryStore store;
    store.set("key", "first");
    store.set("key", "second");
    TEST_ASSERT_EQUAL_STRING("second", store.get("key")->c_str());
}

void test_credential_store_reports_absence_initially() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);

    TEST_ASSERT_FALSE(credentials.hasCertificate());
    TEST_ASSERT_FALSE(credentials.hasPrivateKey());
    TEST_ASSERT_EQUAL_STRING("certificate=absent private_key=absent", credentials.describeForLogging().c_str());
}

void test_credential_store_save_load_clear() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);

    credentials.saveCertificate("cert-pem");
    credentials.savePrivateKey("key-pem");

    TEST_ASSERT_TRUE(credentials.hasCertificate());
    TEST_ASSERT_TRUE(credentials.hasPrivateKey());
    TEST_ASSERT_EQUAL_STRING("cert-pem", credentials.loadCertificate()->c_str());
    TEST_ASSERT_EQUAL_STRING("key-pem", credentials.loadPrivateKey()->c_str());
    TEST_ASSERT_EQUAL_STRING("certificate=present private_key=present", credentials.describeForLogging().c_str());

    credentials.clear();
    TEST_ASSERT_FALSE(credentials.hasCertificate());
    TEST_ASSERT_FALSE(credentials.hasPrivateKey());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_memory_store_basic_get_set);
    RUN_TEST(test_memory_store_remove);
    RUN_TEST(test_memory_store_overwrite);
    RUN_TEST(test_credential_store_reports_absence_initially);
    RUN_TEST(test_credential_store_save_load_clear);
    return UNITY_END();
}
