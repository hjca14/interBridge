#include <cstring>
#include <string>

#include <unity.h>

#include "../../src/dev/dev_command_environment.h"
#include "../../src/hardware/clock.h"
#include "../../src/network/mqtt_topics.h"
#include "../../src/network/mqtt_transport.h"

using namespace interbridge;

// Regression coverage for the Phase 3B.8 cumulative-pass gap: before
// DevCommandEnvironment existed, esp32-c3-dev-ring-simulator built its own
// composition by hand and simply never wired up subscribe()/
// processPending() at all, so a real OPEN_DOOR was never received (see
// docs/dev-ring-simulator.md > "Command processing"). These tests exercise
// the one shared class both esp32-c3-dev-mqtt and
// esp32-c3-dev-ring-simulator now use, so a future omission of the
// commands-topic subscription or the pending-command drain in either entry
// point can no longer hide behind the individually-passing
// CommandHandler/RemoteCommandProcessor unit suites (test_command_handler,
// test_remote_command_processor), which only ever exercised those classes
// in isolation and could not see that the ring simulator never constructed
// them at all.

namespace {

constexpr const char *kDeviceId = "ib-0123456789abcdef0123456789abcdef";
constexpr const char *kCommandId = "abcdef0123456789abcdef0123456789";

std::string openDoorCommandJson(const std::string &commandId = kCommandId) {
  return std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
         "\",\"command_id\":\"" + commandId +
         "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
         "\"expires_at\":1030}";
}

struct Fixture {
  Fixture() : topics(devMqttTopicsConfig(kDeviceId)), env(kDeviceId, clock, transport, topics) {
    clock.setUnixTimeSeconds(1000);
    transport.connect(kDeviceId);
  }

  FakeClock clock;
  FakeDeviceTransport transport;
  MqttTopics topics;
  DevCommandEnvironment env;
};

} // namespace

void setUp() {}
void tearDown() {}

// Proves DevCommandEnvironment::subscribe() actually subscribes to the
// exact commands topic, at QoS 1 - the ring simulator's original bug was
// that no subscription happened at all.
void test_subscribes_to_the_commands_topic() {
  Fixture fixture;

  TEST_ASSERT_TRUE(fixture.env.subscribe());

  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());
  TEST_ASSERT_EQUAL_STRING(fixture.topics.commands().c_str(),
                           fixture.transport.subscriptions()[0].topic.c_str());
  TEST_ASSERT_EQUAL(static_cast<int>(MqttQos::AtLeastOnce),
                    static_cast<int>(fixture.transport.subscriptions()[0].qos));
}

// A valid OPEN_DOOR delivered on the subscribed topic reaches ACCEPTED,
// then - once the deferred terminal is drained on the next call -
// REJECTED/CAPABILITY_DISABLED. Never COMPLETED: DoorOpenCapability stays
// Disabled inside the shared composition, so no DEV entry point using this
// class can accidentally regain real door actuation.
void test_valid_open_door_reaches_accepted_then_capability_disabled() {
  Fixture fixture;
  TEST_ASSERT_TRUE(fixture.env.subscribe());

  fixture.transport.deliver(fixture.topics.commands(), openDoorCommandJson());
  // Delivery happens inside the MQTT callback context; the response is only
  // ever published from an explicit processPending() call, never from
  // deliver() itself.
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());

  fixture.env.processPending(); // publishes ACCEPTED, defers the terminal
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_EQUAL(1, fixture.env.pendingResponseCount());

  fixture.env.processPending(); // drains the deferred terminal
  TEST_ASSERT_EQUAL(0, fixture.env.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "REJECTED"));
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "CAPABILITY_DISABLED"));
  TEST_ASSERT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "COMPLETED"));
  TEST_ASSERT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "COMPLETED"));
}

// Both DEV mains tear down and re-subscribe after every reconnect (their
// shared `subscribed` bookkeeping never assumes a previous subscription
// survives one) - proves the shared class genuinely re-subscribes rather
// than silently reusing stale subscription state, and that a command
// delivered after resubscription is still processed correctly.
void test_resubscribes_after_simulated_reconnect_and_still_processes_commands() {
  Fixture fixture;
  TEST_ASSERT_TRUE(fixture.env.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());

  fixture.transport.disconnect();
  TEST_ASSERT_EQUAL(0, fixture.transport.subscriptionCount());
  fixture.transport.connect(kDeviceId);

  TEST_ASSERT_TRUE(fixture.env.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());

  fixture.transport.deliver(fixture.topics.commands(), openDoorCommandJson());
  fixture.env.processPending();
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
}

// setDiagnosticCallback() must reach the underlying processor - both DEV
// mains rely on this to wire their own Serial log prefix.
void test_diagnostic_callback_forwards_to_the_underlying_processor() {
  Fixture fixture;
  bool sawReceived = false;
  fixture.env.setDiagnosticCallback([&sawReceived](const CommandDiagnostic &event) {
    if (event.stage == CommandDiagnosticStage::Received) sawReceived = true;
  });
  TEST_ASSERT_TRUE(fixture.env.subscribe());

  fixture.transport.deliver(fixture.topics.commands(), openDoorCommandJson());
  fixture.env.processPending();

  TEST_ASSERT_TRUE(sawReceived);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_to_the_commands_topic);
  RUN_TEST(test_valid_open_door_reaches_accepted_then_capability_disabled);
  RUN_TEST(test_resubscribes_after_simulated_reconnect_and_still_processes_commands);
  RUN_TEST(test_diagnostic_callback_forwards_to_the_underlying_processor);
  return UNITY_END();
}
