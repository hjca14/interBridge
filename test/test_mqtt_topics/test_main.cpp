#include <unity.h>

#include "../../src/network/mqtt_topics.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_client_id_equals_device_id() {
    TEST_ASSERT_EQUAL_STRING("ib-abc123", MqttTopics::clientId("ib-abc123").c_str());
}

void test_commands_topic() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    MqttTopics topics(config);
    TEST_ASSERT_EQUAL_STRING("interbridge/ib-abc123/commands", topics.commands().c_str());
}

void test_basic_ingest_topics_use_configured_rule_names() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    config.ingestRuleName = "my_ingest_rule";
    config.responseRuleName = "my_response_rule";
    MqttTopics topics(config);

    TEST_ASSERT_EQUAL_STRING("$aws/rules/my_ingest_rule/interbridge/ib-abc123/events", topics.eventsIngest().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/my_ingest_rule/interbridge/ib-abc123/health", topics.healthIngest().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/rules/my_response_rule/interbridge/ib-abc123/responses",
                              topics.responsesIngest().c_str());
}

void test_dev_rule_names_match_backend_contract() {
    auto config = devMqttTopicsConfig("ib-abc123");
    TEST_ASSERT_EQUAL_STRING("interbridge_dev_ingest_rule", config.ingestRuleName.c_str());
    TEST_ASSERT_EQUAL_STRING("interbridge_dev_response_rule", config.responseRuleName.c_str());
}

void test_shadow_topics_use_shadow_name() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    config.shadowName = "interbridge";
    MqttTopics topics(config);

    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/shadow/name/interbridge/update", topics.shadowUpdate().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/shadow/name/interbridge/update/accepted",
                              topics.shadowUpdateAccepted().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/shadow/name/interbridge/update/rejected",
                              topics.shadowUpdateRejected().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/shadow/name/interbridge/update/delta",
                              topics.shadowUpdateDelta().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/shadow/name/interbridge/get", topics.shadowGet().c_str());
}

void test_jobs_topics() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    MqttTopics topics(config);

    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/jobs/notify-next", topics.jobsNotifyNext().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/jobs/$next/get", topics.jobsNextGet().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/jobs/job-1/update", topics.jobsUpdate("job-1").c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/things/ib-abc123/jobs/job-1/update/accepted",
                              topics.jobsUpdateAccepted("job-1").c_str());
}

void test_fleet_provisioning_topics() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    MqttTopics topics(config);

    TEST_ASSERT_EQUAL_STRING("$aws/certificates/create-from-csr/json", topics.fleetProvisioningCreateCertFromCsr().c_str());
    TEST_ASSERT_EQUAL_STRING("$aws/certificates/create-from-csr/json/accepted",
                              topics.fleetProvisioningCreateCertFromCsrAccepted().c_str());
}

void test_fleet_provisioning_register_thing_empty_when_template_not_configured() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    // fleetProvisioningTemplateName left empty (not decided yet).
    MqttTopics topics(config);

    TEST_ASSERT_TRUE(topics.fleetProvisioningRegisterThing().empty());
}

void test_fleet_provisioning_register_thing_uses_template_name_when_set() {
    MqttTopicsConfig config;
    config.deviceId = "ib-abc123";
    config.fleetProvisioningTemplateName = "InterBridgeTemplate";
    MqttTopics topics(config);

    TEST_ASSERT_EQUAL_STRING("$aws/provisioning-templates/InterBridgeTemplate/provision/json",
                              topics.fleetProvisioningRegisterThing().c_str());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_client_id_equals_device_id);
    RUN_TEST(test_commands_topic);
    RUN_TEST(test_basic_ingest_topics_use_configured_rule_names);
    RUN_TEST(test_dev_rule_names_match_backend_contract);
    RUN_TEST(test_shadow_topics_use_shadow_name);
    RUN_TEST(test_jobs_topics);
    RUN_TEST(test_fleet_provisioning_topics);
    RUN_TEST(test_fleet_provisioning_register_thing_empty_when_template_not_configured);
    RUN_TEST(test_fleet_provisioning_register_thing_uses_template_name_when_set);
    return UNITY_END();
}
