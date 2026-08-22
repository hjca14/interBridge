#include <unity.h>
#include "../../src/network/mqtt_topics.h"

using namespace interbridge;
namespace { constexpr const char* kId = "ib-dev-test-placeholder"; }
void setUp() {}
void tearDown() {}

void test_dev_contract_topics_and_client_id() {
    MqttTopics topics(devMqttTopicsConfig(kId));
    TEST_ASSERT_EQUAL_STRING(kId, MqttTopics::clientId(kId).c_str());
    TEST_ASSERT_EQUAL_STRING("interbridge/ib-dev-test-placeholder/commands", topics.commands().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/interbridge_dev_response_rule/interbridge/ib-dev-test-placeholder/responses", topics.responsesIngest().c_str());
    TEST_ASSERT_EQUAL(std::string::npos, topics.commands().find('#'));
    TEST_ASSERT_EQUAL(std::string::npos, topics.commands().find('+'));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_dev_contract_topics_and_client_id);
    return UNITY_END();
}
