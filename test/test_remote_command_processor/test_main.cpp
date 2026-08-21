#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "../../src/core/logger.h"
#include "../../src/hardware/clock.h"
#include "../../src/hardware/gpio.h"
#include "../../src/hardware/system_control.h"
#include "../../src/intercom/intercom.h"
#include "../../src/network/mqtt_topics.h"
#include "../../src/network/mqtt_transport.h"
#include "../../src/protocol/command_cache.h"
#include "../../src/protocol/command_handler.h"
#include "../../src/protocol/remote_command_processor.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

namespace {

constexpr const char *kDeviceId = "ib-0123456789abcdef0123456789abcdef";
constexpr const char *kCommandId = "abcdef0123456789abcdef0123456789";
std::vector<std::string> capturedLogs;

class ObservingHardware : public IHardwareIO {
public:
  bool readLineState() override { return false; }

  bool setDoorOutput(bool) override {
    doorOutputCalls++;
    return true;
  }

  int doorOutputCalls = 0;
};

void captureLog(const char *line) { capturedLogs.emplace_back(line); }

std::string commandJson(const std::string &overrides = "") {
  if (!overrides.empty()) {
    return overrides;
  }
  return std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
         "\",\"command_id\":\"" + kCommandId +
         "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
         "\"expires_at\":1030}";
}

struct Fixture {
  Fixture()
      : intercom(hardware), cache(store),
        handler(kDeviceId, clock, cache, intercom, systemControl),
        topics(devMqttTopicsConfig(kDeviceId)),
        processor(kDeviceId, transport, handler, topics) {
    clock.setUnixTimeSeconds(1000);
    transport.connect(kDeviceId);
  }

  ObservingHardware hardware;
  Intercom intercom;
  FakeClock clock;
  MemoryStore store;
  PersistentDedupCache cache;
  FakeSystemControl systemControl;
  FakeDeviceTransport transport;
  MqttTopics topics;
  CommandHandler handler;
  RemoteCommandProcessor processor;
};

void assertInvalid(const std::string &payload) {
  CommandParseResult result = parseCommand(payload, kDeviceId);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(CommandParseStatus::Ok),
                        static_cast<int>(result.status));
}

} // namespace

void setUp() {
  capturedLogs.clear();
  Logger::setSink(captureLog);
}

void tearDown() { Logger::setSink(nullptr); }

void test_valid_open_door_publishes_accepted_then_capability_disabled() {
  Fixture fixture;

  CommandPublishResult result = fixture.processor.processPayload(commandJson());

  TEST_ASSERT_TRUE(result.parsed);
  TEST_ASSERT_TRUE(result.acceptedPublished);
  TEST_ASSERT_TRUE(result.terminalPublished);
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "REJECTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
  TEST_ASSERT_EQUAL(0, fixture.hardware.doorOutputCalls);
  TEST_ASSERT_EQUAL(0, fixture.systemControl.restartCount());
}

void test_duplicate_publishes_only_stored_terminal_response() {
  Fixture fixture;

  fixture.processor.processPayload(commandJson());
  CommandPublishResult duplicate =
      fixture.processor.processPayload(commandJson());

  TEST_ASSERT_FALSE(duplicate.acceptedPublished);
  TEST_ASSERT_TRUE(duplicate.terminalPublished);
  TEST_ASSERT_EQUAL(3, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[2].payload.c_str(),
             "CAPABILITY_DISABLED"));
  TEST_ASSERT_EQUAL(0, fixture.hardware.doorOutputCalls);
}

void test_deduplication_survives_handler_and_cache_reconstruction() {
  Fixture fixture;
  fixture.processor.processPayload(commandJson());
  PersistentDedupCache rebuiltCache(fixture.store);
  CommandHandler rebuiltHandler(kDeviceId, fixture.clock, rebuiltCache,
                                fixture.intercom, fixture.systemControl);
  RemoteCommandProcessor rebuiltProcessor(kDeviceId, fixture.transport,
                                          rebuiltHandler, fixture.topics);

  CommandPublishResult duplicate =
      rebuiltProcessor.processPayload(commandJson());

  TEST_ASSERT_FALSE(duplicate.acceptedPublished);
  TEST_ASSERT_TRUE(duplicate.terminalPublished);
  TEST_ASSERT_EQUAL(0, fixture.hardware.doorOutputCalls);
}

void test_strict_parser_rejects_invalid_contract_payloads() {
  assertInvalid("not json");
  assertInvalid(std::string(kMaxJsonPayloadBytes + 1, 'x'));
  assertInvalid(std::string("{\"protocol_version\":2,\"device_id\":\"") +
                kDeviceId + "\"}");
  assertInvalid(
      std::string("{\"protocol_version\":1,\"device_id\":\"ib-"
                  "ffffffffffffffffffffffffffffffff\",\"command_id\":\"") +
      kCommandId +
      "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
      "\"expires_at\":1030}");
  assertInvalid(std::string("{\"protocol_version\":1,\"device_id\":\"") +
                kDeviceId +
                "\",\"command_id\":\"ABC\",\"command\":\"OPEN_DOOR\","
                "\"parameters\":{},\"issued_at\":1000,\"expires_at\":1030}");
  assertInvalid(
      std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
      "\",\"command_id\":\"" + kCommandId +
      "\",\"command\":\"OPEN_DOOR\",\"issued_at\":1000,\"expires_at\":1030}");
  assertInvalid(std::string("{\"protocol_version\":1,\"device_id\":\"") +
                kDeviceId + "\",\"command_id\":\"" + kCommandId +
                "\",\"command\":\"OPEN_DOOR\",\"parameters\":{\"key\":\"1\"},"
                "\"issued_at\":1000,\"expires_at\":1030}");
  assertInvalid(std::string("{\"protocol_version\":1,\"device_id\":\"") +
                kDeviceId + "\",\"command_id\":\"" + kCommandId +
                "\",\"command\":\"OPEN_DOOR\",\"parameters\":[],"
                "\"issued_at\":1000,\"expires_at\":1030}");
}

void test_strict_parser_accepts_exactly_the_seven_contract_fields() {
  CommandParseResult result = parseCommand(commandJson(), kDeviceId);

  TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok),
                    static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_STRING(kDeviceId, result.command.deviceId.c_str());
  TEST_ASSERT_EQUAL_STRING(kCommandId, result.command.commandId.c_str());
}

void test_strict_parser_rejects_legacy_payload_field() {
  std::string payload =
      std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
      "\",\"command_id\":\"" + kCommandId +
      "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
      "\"expires_at\":1030,\"payload\":{}}";

  assertInvalid(payload);
}

void test_strict_parser_rejects_unknown_field() {
  std::string payload =
      std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
      "\",\"command_id\":\"" + kCommandId +
      "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
      "\"expires_at\":1030,\"unexpected\":true}";

  assertInvalid(payload);
}

void test_legacy_parser_preserves_payload_compatibility() {
  std::string payload =
      std::string("{\"protocol_version\":1,\"command_id\":\"") + kCommandId +
      "\",\"command\":\"UPDATE_FIRMWARE\",\"payload\":{\"version\":\"0.2.0\"}}";

  CommandParseResult result = parseCommand(payload);

  TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok),
                    static_cast<int>(result.status));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        result.command.rawPayload.find("0.2.0"));
}

void test_physical_fields_are_rejected() {
  const char *fields[] = {"dtmf",  "key",   "gpio",          "mode",
                          "relay", "pulse", "pulse_duration"};
  for (const char *field : fields) {
    std::string payload =
        std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
        "\",\"command_id\":\"" + kCommandId +
        "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
        "\"expires_at\":1030,\"" +
        field + "\":1}";
    assertInvalid(payload);
  }
}

void test_publish_failures_are_observable() {
  Fixture firstFailure;
  firstFailure.transport.armPublishFailure(1);
  CommandPublishResult first =
      firstFailure.processor.processPayload(commandJson());
  TEST_ASSERT_FALSE(first.acceptedPublished);
  TEST_ASSERT_FALSE(first.terminalPublished);

  Fixture secondFailure;
  secondFailure.transport.armPublishFailureOnCall(2);
  CommandPublishResult second =
      secondFailure.processor.processPayload(commandJson());
  TEST_ASSERT_TRUE(second.acceptedPublished);
  TEST_ASSERT_FALSE(second.terminalPublished);
}

void test_exact_topic_qos_callback_and_resubscription() {
  Fixture fixture;
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());

  fixture.transport.deliver(fixture.topics.commands(), commandJson());

  TEST_ASSERT_TRUE(fixture.processor.lastResult().terminalPublished);
  TEST_ASSERT_EQUAL_STRING(
      "interbridge/ib-0123456789abcdef0123456789abcdef/commands",
      fixture.topics.commands().c_str());
  TEST_ASSERT_EQUAL(
      static_cast<int>(MqttQos::AtLeastOnce),
      static_cast<int>(fixture.transport.publishedMessages()[0].qos));
  fixture.transport.disconnect();
  TEST_ASSERT_EQUAL(0, fixture.transport.subscriptionCount());
  fixture.transport.connect(kDeviceId);
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());
}

void test_logs_never_contain_raw_payload_or_identifiers() {
  Fixture fixture;
  std::string secretMarker = "raw-secret-marker";
  fixture.processor.processPayload(secretMarker);

  for (const std::string &line : capturedLogs) {
    TEST_ASSERT_EQUAL(std::string::npos, line.find(secretMarker));
    TEST_ASSERT_EQUAL(std::string::npos, line.find(kCommandId));
    TEST_ASSERT_EQUAL(std::string::npos, line.find(kDeviceId));
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_valid_open_door_publishes_accepted_then_capability_disabled);
  RUN_TEST(test_duplicate_publishes_only_stored_terminal_response);
  RUN_TEST(test_deduplication_survives_handler_and_cache_reconstruction);
  RUN_TEST(test_strict_parser_rejects_invalid_contract_payloads);
  RUN_TEST(test_strict_parser_accepts_exactly_the_seven_contract_fields);
  RUN_TEST(test_strict_parser_rejects_legacy_payload_field);
  RUN_TEST(test_strict_parser_rejects_unknown_field);
  RUN_TEST(test_legacy_parser_preserves_payload_compatibility);
  RUN_TEST(test_physical_fields_are_rejected);
  RUN_TEST(test_publish_failures_are_observable);
  RUN_TEST(test_exact_topic_qos_callback_and_resubscription);
  RUN_TEST(test_logs_never_contain_raw_payload_or_identifiers);
  return UNITY_END();
}
