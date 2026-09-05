#include <unity.h>

#include "../../src/dev/dev_dns_readiness.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

// The regression this guards: a connected STA interface with a valid
// local IP must be sufficient to attempt DNS resolution, regardless of
// whether a separate "DNS server configured" signal (WiFi.dnsIP() on
// Arduino-ESP32) is also true - see docs/dev-ble-mqtt.md's "DNS
// precondition" note for the real bench failure this was extracted from.
void test_ready_when_wifi_connected_and_local_ip_present() {
    TEST_ASSERT_TRUE(isReadyToAttemptDnsResolution(true, true));
}

void test_not_ready_when_wifi_not_connected() {
    TEST_ASSERT_FALSE(isReadyToAttemptDnsResolution(false, true));
}

void test_not_ready_when_local_ip_missing() {
    TEST_ASSERT_FALSE(isReadyToAttemptDnsResolution(true, false));
}

void test_not_ready_when_neither_condition_holds() {
    TEST_ASSERT_FALSE(isReadyToAttemptDnsResolution(false, false));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_ready_when_wifi_connected_and_local_ip_present);
    RUN_TEST(test_not_ready_when_wifi_not_connected);
    RUN_TEST(test_not_ready_when_local_ip_missing);
    RUN_TEST(test_not_ready_when_neither_condition_holds);
    return UNITY_END();
}
