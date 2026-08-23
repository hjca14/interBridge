#include <unity.h>

#include "../../src/network/mqtt_transport.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

namespace {
class FakeMqttClient : public IMqttClient {
public:
  bool configureTls(const std::string &endpointValue, uint16_t,
                    const std::string &caValue, const std::string &certValue,
                    const std::string &keyValue, uint16_t keepAlive,
                    uint16_t timeout) override {
    endpoint = endpointValue;
    ca = caValue;
    cert = certValue;
    key = keyValue;
    configuredKeepAlive = keepAlive;
    configuredTimeout = timeout;
    return configureResult;
  }
  void setMessageCallback(MqttMessageCallback value) override {
    callback = std::move(value);
  }
  bool connect(const std::string &value) override {
    clientId = value;
    isConnected = connectResult;
    return connectResult;
  }
  void disconnect() override {
    isConnected = false;
    ++disconnectCalls;
  }
  bool connected() override { return isConnected; }
  bool publish(const std::string &, const std::string &, MqttQos qos,
               bool retain) override {
    publishedQos = qos;
    publishedRetain = retain;
    return publishResult;
  }
  bool subscribe(const std::string &value, MqttQos qos) override {
    subscribedTopic = value;
    subscribedQos = qos;
    return subscribeResult;
  }
  void poll() override {
    ++polls;
    // Simulates MQTTClient::loop() closing the connection internally after
    // a keepalive/read failure (see MQTTClient::close() in the vendored
    // 256dpi/MQTT library).
    if (pollBreaksConnection) isConnected = false;
  }
  bool configureResult = true, connectResult = true, publishResult = true,
       subscribeResult = true, isConnected = false, pollBreaksConnection = false;
  bool publishedRetain = true;
  int polls = 0;
  int disconnectCalls = 0;
  uint16_t configuredKeepAlive = 0, configuredTimeout = 0;
  MqttQos publishedQos = MqttQos::AtMostOnce,
          subscribedQos = MqttQos::AtMostOnce;
  std::string endpoint, ca, cert, key, clientId, subscribedTopic;
  MqttMessageCallback callback;
};
constexpr const char *kValidId = "ib-0123456789abcdef0123456789abcdef";
AwsIotConnectionConfig validConfig() {
  AwsIotConnectionConfig c;
  c.endpoint = "example-ats.iot.eu-west-1.amazonaws.com";
  c.rootCaPem = "LOCAL_CA";
  return c;
}
} // namespace

void test_real_transport_configures_mtls_and_exact_client_id() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("LOCAL_CERT");
  credentials.savePrivateKey("LOCAL_KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);
  TEST_ASSERT_TRUE(transport.connect(kValidId));
  TEST_ASSERT_EQUAL_STRING(kValidId, mqtt.clientId.c_str());
  TEST_ASSERT_EQUAL_STRING("LOCAL_CERT", mqtt.cert.c_str());
  TEST_ASSERT_EQUAL_STRING("LOCAL_KEY", mqtt.key.c_str());
  TEST_ASSERT_EQUAL(30, mqtt.configuredKeepAlive);
  TEST_ASSERT_EQUAL(1500, mqtt.configuredTimeout);
}

void test_real_transport_rejects_invalid_or_missing_configuration() {
  MemoryStore emptyStore;
  DeviceCredentialStore emptyCredentials(emptyStore);
  FakeMqttClient mqtt;
  Esp32AwsIotTransport missing(validConfig(), emptyCredentials, mqtt);
  TEST_ASSERT_FALSE(missing.connect(kValidId));
  TEST_ASSERT_TRUE(mqtt.clientId.empty());
  AwsIotConnectionConfig invalid = validConfig();
  invalid.endpoint = "https://bad.example";
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  Esp32AwsIotTransport bad(invalid, credentials, mqtt);
  TEST_ASSERT_FALSE(bad.connect(kValidId));
  TEST_ASSERT_FALSE(bad.connect("other-device"));
}

void test_real_transport_qos_retain_subscribe_failure_and_disconnect() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);
  TEST_ASSERT_TRUE(transport.connect(kValidId));
  mqtt.subscribeResult = false;
  TEST_ASSERT_FALSE(
      transport.subscribe("interbridge/x/commands", MqttQos::AtLeastOnce,
                          [](const std::string &, const std::string &) {}));
  // A subscribe failure invalidates the session (see
  // Esp32AwsIotTransport::isConnected()) - it must never be reusable again
  // without an explicit reconnect, even once the underlying client would
  // otherwise succeed.
  TEST_ASSERT_FALSE(transport.isConnected());
  mqtt.subscribeResult = true;
  TEST_ASSERT_FALSE(
      transport.subscribe("interbridge/x/commands", MqttQos::AtLeastOnce,
                          [](const std::string &, const std::string &) {}));

  TEST_ASSERT_TRUE(transport.connect(kValidId));
  TEST_ASSERT_TRUE(
      transport.subscribe("interbridge/x/commands", MqttQos::AtLeastOnce,
                          [](const std::string &, const std::string &) {}));
  TEST_ASSERT_EQUAL(static_cast<int>(MqttQos::AtLeastOnce),
                    static_cast<int>(mqtt.subscribedQos));
  TEST_ASSERT_TRUE(
      transport.publish("topic", "{}", MqttQos::AtLeastOnce, false));
  TEST_ASSERT_FALSE(mqtt.publishedRetain);
  transport.disconnect();
  TEST_ASSERT_FALSE(transport.isConnected());
}

void test_publish_failure_marks_transport_invalid_even_if_client_still_reports_connected() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);
  TEST_ASSERT_TRUE(transport.connect(kValidId));

  mqtt.publishResult = false;
  TEST_ASSERT_FALSE(transport.publish("topic", "{}", MqttQos::AtLeastOnce));
  // The transport must not rely solely on the underlying client's own
  // connected() bookkeeping - a failed publish invalidates the session even
  // if the (possibly stale) underlying client still reports connected.
  TEST_ASSERT_TRUE(mqtt.isConnected);
  TEST_ASSERT_FALSE(transport.isConnected());
}

void test_subscribe_failure_marks_transport_invalid() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);
  TEST_ASSERT_TRUE(transport.connect(kValidId));

  mqtt.subscribeResult = false;
  TEST_ASSERT_FALSE(
      transport.subscribe("interbridge/x/commands", MqttQos::AtLeastOnce,
                          [](const std::string &, const std::string &) {}));
  TEST_ASSERT_FALSE(transport.isConnected());
}

void test_poll_detects_broken_session() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);
  TEST_ASSERT_TRUE(transport.connect(kValidId));

  mqtt.pollBreaksConnection = true;
  transport.poll();
  TEST_ASSERT_FALSE(transport.isConnected());
}

void test_reconnect_tears_down_before_reusing_invalid_session() {
  MemoryStore store;
  DeviceCredentialStore credentials(store);
  credentials.saveCertificate("CERT");
  credentials.savePrivateKey("KEY");
  FakeMqttClient mqtt;
  Esp32AwsIotTransport transport(validConfig(), credentials, mqtt);

  TEST_ASSERT_TRUE(transport.connect(kValidId));
  const int disconnectsAfterFirstConnect = mqtt.disconnectCalls;
  TEST_ASSERT_TRUE(disconnectsAfterFirstConnect >= 1);

  // A publish failure breaks the session without any explicit disconnect()
  // call from the orchestration loop (mirrors Wi-Fi/time staying valid while
  // only the MQTT/TLS session dies mid-command).
  mqtt.publishResult = false;
  TEST_ASSERT_FALSE(transport.publish("topic", "{}", MqttQos::AtLeastOnce));
  TEST_ASSERT_FALSE(transport.isConnected());
  mqtt.publishResult = true;

  // Reconnecting must tear the old session down first, never reuse it as-is.
  TEST_ASSERT_TRUE(transport.connect(kValidId));
  TEST_ASSERT_TRUE(mqtt.disconnectCalls > disconnectsAfterFirstConnect);
  TEST_ASSERT_TRUE(transport.isConnected());
}

void test_connect_succeeds_and_records_client_id() {
  FakeDeviceTransport transport;
  TEST_ASSERT_TRUE(transport.connect("ib-abc123"));
  TEST_ASSERT_TRUE(transport.isConnected());
  TEST_ASSERT_EQUAL_STRING("ib-abc123", transport.lastClientId().c_str());
}

void test_publish_fails_when_not_connected() {
  FakeDeviceTransport transport;
  TEST_ASSERT_FALSE(
      transport.publish("topic", "payload", MqttQos::AtLeastOnce));
}

void test_publish_records_message_when_connected() {
  FakeDeviceTransport transport;
  transport.connect("ib-abc123");
  transport.publish("interbridge/ib-abc123/events", "{}", MqttQos::AtLeastOnce);

  TEST_ASSERT_EQUAL(1, static_cast<int>(transport.publishedMessages().size()));
  TEST_ASSERT_EQUAL_STRING("interbridge/ib-abc123/events",
                           transport.publishedMessages()[0].topic.c_str());
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
  transport.subscribe(
      "interbridge/ib-abc123/commands", MqttQos::AtLeastOnce,
      [&](const std::string &topic, const std::string &payload) {
        receivedTopic = topic;
        receivedPayload = payload;
      });

  transport.deliver("interbridge/ib-abc123/commands",
                    "{\"command\":\"OPEN_DOOR\"}");

  TEST_ASSERT_EQUAL_STRING("interbridge/ib-abc123/commands",
                           receivedTopic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"command\":\"OPEN_DOOR\"}",
                           receivedPayload.c_str());
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_connect_succeeds_and_records_client_id);
  RUN_TEST(test_publish_fails_when_not_connected);
  RUN_TEST(test_publish_records_message_when_connected);
  RUN_TEST(test_armed_connect_failures_are_consumed_then_succeed);
  RUN_TEST(test_subscribe_and_deliver_invokes_callback);
  RUN_TEST(test_real_transport_configures_mtls_and_exact_client_id);
  RUN_TEST(test_real_transport_rejects_invalid_or_missing_configuration);
  RUN_TEST(test_real_transport_qos_retain_subscribe_failure_and_disconnect);
  RUN_TEST(test_publish_failure_marks_transport_invalid_even_if_client_still_reports_connected);
  RUN_TEST(test_subscribe_failure_marks_transport_invalid);
  RUN_TEST(test_poll_detects_broken_session);
  RUN_TEST(test_reconnect_tears_down_before_reusing_invalid_session);
  return UNITY_END();
}
