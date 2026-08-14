#include <unity.h>
#include "../../src/dev/mqtt_smoke_handler.h"
#include "../../src/network/mqtt_topics.h"
#include "../../src/hardware/clock.h"

using namespace interbridge;
namespace { constexpr const char* kId = "ib-dev-test-placeholder"; constexpr const char* kCmd = "0123456789abcdef0123456789abcdef"; }
void setUp() {}
void tearDown() {}

std::string command(int64_t issued, int64_t expires) {
    return R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":")" + std::string(kCmd) +
           R"(","issued_at":)" + std::to_string(issued) + R"(,"expires_at":)" + std::to_string(expires) + "}";
}

void test_dev_contract_topics_and_client_id() {
    MqttTopics topics(devMqttTopicsConfig(kId));
    TEST_ASSERT_EQUAL_STRING(kId, MqttTopics::clientId(kId).c_str());
    TEST_ASSERT_EQUAL_STRING("interbridge/ib-dev-test-placeholder/commands", topics.commands().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/interbridge_dev_ingest_rule/interbridge/ib-dev-test-placeholder/events", topics.eventsIngest().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/interbridge_dev_ingest_rule/interbridge/ib-dev-test-placeholder/health", topics.healthIngest().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/interbridge_dev_response_rule/interbridge/ib-dev-test-placeholder/responses", topics.responsesIngest().c_str());
    TEST_ASSERT_FALSE(topics.commands() == "interbridge/another-device/commands");
    TEST_ASSERT_EQUAL(1, kDevSmokeCommandSubscribeQos);
    TEST_ASSERT_EQUAL(0, kDevSmokeHealthPublishQos);
    TEST_ASSERT_EQUAL(1, kDevSmokeEventPublishQos);
    TEST_ASSERT_EQUAL(1, kDevSmokeResponsePublishQos);
    TEST_ASSERT_FALSE(kDevSmokeRetain);
}

void test_valid_command_is_safely_rejected() {
    FakeClock clock; clock.setUnixTimeSeconds(100);
    auto response = DevMqttSmokeHandler(kId, clock).handle(command(95, 105));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CommandNotAllowed), static_cast<int>(response.error->code));
}

void test_malformed_and_oversized_are_rejected() {
    FakeClock clock; clock.setUnixTimeSeconds(100); DevMqttSmokeHandler handler(kId, clock);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::InvalidPayload), static_cast<int>(handler.handle("{").error->code));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::PayloadTooLarge), static_cast<int>(handler.handle(std::string(kMaxJsonPayloadBytes + 1, 'x')).error->code));
}

void test_timestamp_and_clock_fail_closed() {
    FakeClock clock; DevMqttSmokeHandler handler(kId, clock);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::ClockNotTrustworthy), static_cast<int>(handler.handle(command(95, 105)).error->code));
    clock.setUnixTimeSeconds(100);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::InvalidTimestamp), static_cast<int>(handler.handle(command(105, 95)).error->code));
}

int main(int, char**) {
    UNITY_BEGIN(); RUN_TEST(test_dev_contract_topics_and_client_id); RUN_TEST(test_valid_command_is_safely_rejected);
    RUN_TEST(test_malformed_and_oversized_are_rejected); RUN_TEST(test_timestamp_and_clock_fail_closed); return UNITY_END();
}
