#include <unity.h>

#include "../../src/network/mqtt_transport.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_connect_succeeds_and_records_client_id() {
    FakeDeviceTransport transport;
    TEST_ASSERT_TRUE(transport.connect("ib-abc123"));
    TEST_ASSERT_TRUE(transport.isConnected());
    TEST_ASSERT_EQUAL_STRING("ib-abc123", transport.lastClientId().c_str());
}

void test_publish_fails_when_not_connected() {
    FakeDeviceTransport transport;
    TEST_ASSERT_FALSE(transport.publish("topic", "payload", MqttQos::AtLeastOnce));
}

void test_publish_records_message_when_connected() {
    FakeDeviceTransport transport;
    transport.connect("ib-abc123");
    transport.publish("interbridge/ib-abc123/events", "{}", MqttQos::AtLeastOnce);

    TEST_ASSERT_EQUAL(1, static_cast<int>(transport.publishedMessages().size()));
    TEST_ASSERT_EQUAL_STRING("interbridge/ib-abc123/events", transport.publishedMessages()[0].topic.c_str());
}

void test_armed_connect_failures_are_consumed_then_succeed() {
    FakeDeviceTransport transport;
    transport.armConnectFailure(2);

    TEST_ASSERT_FALSE(transport.connect("ib-abc123"));
    TEST_ASSERT_FALSE(transport.connect("ib-abc123"));
    TEST_ASSERT_TRUE(transport.connect("ib-abc123"));
}

void test_subscribe_and_deliver_invokes_callback() {
    FakeDeviceTransport transport;
    transport.connect("ib-abc123");

    std::string receivedTopic;
    std::string receivedPayload;
    transport.subscribe("interbridge/ib-abc123/commands", [&](const std::string& topic, const std::string& payload) {
        receivedTopic = topic;
        receivedPayload = payload;
    });

    transport.deliver("interbridge/ib-abc123/commands", "{\"command\":\"OPEN_DOOR\"}");

    TEST_ASSERT_EQUAL_STRING("interbridge/ib-abc123/commands", receivedTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"OPEN_DOOR\"}", receivedPayload.c_str());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_connect_succeeds_and_records_client_id);
    RUN_TEST(test_publish_fails_when_not_connected);
    RUN_TEST(test_publish_records_message_when_connected);
    RUN_TEST(test_armed_connect_failures_are_consumed_then_succeed);
    RUN_TEST(test_subscribe_and_deliver_invokes_callback);
    return UNITY_END();
}
