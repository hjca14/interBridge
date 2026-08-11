#include <unity.h>

#include "../../src/provisioning/fleet_provisioning.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_successful_provisioning_stores_certificate_and_key() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);
    FakeKeyPairGenerator keyGen;
    FakeFleetProvisioningTransport transport;
    FleetProvisioningCoordinator coordinator(keyGen, transport, credentials, "InterBridgeTemplate");

    FleetProvisioningResult result = coordinator.provision("ib-abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(FleetProvisioningResult::Success), static_cast<int>(result));
    TEST_ASSERT_TRUE(credentials.hasCertificate());
    TEST_ASSERT_TRUE(credentials.hasPrivateKey());
    TEST_ASSERT_EQUAL(1, transport.createCertificateCalls());
    TEST_ASSERT_EQUAL(1, transport.registerThingCalls());
}

void test_key_generation_failure_stops_before_network_calls() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);
    FakeKeyPairGenerator keyGen;
    keyGen.setResult(std::nullopt);
    FakeFleetProvisioningTransport transport;
    FleetProvisioningCoordinator coordinator(keyGen, transport, credentials, "InterBridgeTemplate");

    FleetProvisioningResult result = coordinator.provision("ib-abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(FleetProvisioningResult::KeyGenerationFailed), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, transport.createCertificateCalls());
    TEST_ASSERT_FALSE(credentials.hasCertificate());
}

void test_certificate_request_failure_stops_before_register() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);
    FakeKeyPairGenerator keyGen;
    FakeFleetProvisioningTransport transport;
    transport.setCreateCertificateResult(std::nullopt);
    FleetProvisioningCoordinator coordinator(keyGen, transport, credentials, "InterBridgeTemplate");

    FleetProvisioningResult result = coordinator.provision("ib-abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(FleetProvisioningResult::CertificateRequestFailed), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, transport.registerThingCalls());
    TEST_ASSERT_FALSE(credentials.hasCertificate());
}

void test_register_thing_failure_does_not_store_credentials() {
    MemoryStore store;
    DeviceCredentialStore credentials(store);
    FakeKeyPairGenerator keyGen;
    FakeFleetProvisioningTransport transport;
    transport.setRegisterThingResult(false);
    FleetProvisioningCoordinator coordinator(keyGen, transport, credentials, "InterBridgeTemplate");

    FleetProvisioningResult result = coordinator.provision("ib-abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(FleetProvisioningResult::RegisterThingFailed), static_cast<int>(result));
    TEST_ASSERT_FALSE(credentials.hasCertificate());
    TEST_ASSERT_FALSE(credentials.hasPrivateKey());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_successful_provisioning_stores_certificate_and_key);
    RUN_TEST(test_key_generation_failure_stops_before_network_calls);
    RUN_TEST(test_certificate_request_failure_stops_before_register);
    RUN_TEST(test_register_thing_failure_does_not_store_credentials);
    return UNITY_END();
}
