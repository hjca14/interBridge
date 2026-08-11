#include <string>

#include <unity.h>

#include "../../src/aws/device_shadow.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_reported_update_includes_expected_fields() {
    ShadowReportedState state;
    state.firmwareVersion = "0.1.0";
    state.hardwareVersion = "1.0";
    state.intercomState = "IDLE";
    state.wifiRssi = -54;
    state.uptimeMs = 3812000;
    state.provisioned = true;
    state.healthIntervalSeconds = 3600;

    std::string json = DeviceShadow::buildReportedUpdate(state);

    TEST_ASSERT_TRUE(json.find("\"reported\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("0.1.0") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("IDLE") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("-54") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("3812000") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("true") != std::string::npos);
}

void test_parse_delta_with_nested_state_object() {
    auto delta = DeviceShadow::parseDelta(
        R"({"version":12,"timestamp":1699999999,"state":{"health_interval_s":120}})");

    TEST_ASSERT_TRUE(delta.healthIntervalChanged);
    TEST_ASSERT_EQUAL(120, static_cast<int>(delta.healthIntervalSeconds));
}

void test_parse_delta_ignores_unknown_fields() {
    auto delta = DeviceShadow::parseDelta(
        R"({"state":{"ring_timeout_ms":5000,"audio_volume":80}})");

    TEST_ASSERT_FALSE(delta.healthIntervalChanged);
}

void test_parse_delta_on_malformed_json_fails_safe() {
    auto delta = DeviceShadow::parseDelta("not json at all {{{");
    TEST_ASSERT_FALSE(delta.healthIntervalChanged);
}

void test_parse_delta_on_empty_payload_fails_safe() {
    auto delta = DeviceShadow::parseDelta("{}");
    TEST_ASSERT_FALSE(delta.healthIntervalChanged);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_reported_update_includes_expected_fields);
    RUN_TEST(test_parse_delta_with_nested_state_object);
    RUN_TEST(test_parse_delta_ignores_unknown_fields);
    RUN_TEST(test_parse_delta_on_malformed_json_fails_safe);
    RUN_TEST(test_parse_delta_on_empty_payload_fails_safe);
    return UNITY_END();
}
