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

std::string commandJsonWithId(const std::string &commandId) {
  return std::string("{\"protocol_version\":1,\"device_id\":\"") + kDeviceId +
         "\",\"command_id\":\"" + commandId +
         "\",\"command\":\"OPEN_DOOR\",\"parameters\":{},\"issued_at\":1000,"
         "\"expires_at\":1030}";
}

// Wraps a real IDedupCache and counts calls, so a test can prove
// CommandHandler is consulted exactly once per command even when its
// already-computed responses are retried multiple times out of the outbox.
class CountingDedupCache : public IDedupCache {
public:
  explicit CountingDedupCache(IDedupCache &inner) : inner_(inner) {}
  std::optional<DedupEntry> find(const std::string &commandId) override {
    ++findCalls;
    return inner_.find(commandId);
  }
  void record(const std::string &commandId, const DedupEntry &entry) override {
    ++recordCalls;
    inner_.record(commandId, entry);
  }
  int findCalls = 0;
  int recordCalls = 0;

private:
  IDedupCache &inner_;
};

// Wraps a real IDeviceTransport and counts publish() calls, so a test can
// prove no single RemoteCommandProcessor call path (processPayload() for a
// new command, or processPending() draining the outbox) ever attempts more
// than one response publish.
class CountingTransport : public IDeviceTransport {
public:
  explicit CountingTransport(IDeviceTransport &inner) : inner_(inner) {}
  bool connect(const std::string &clientId) override {
    return inner_.connect(clientId);
  }
  void disconnect() override { inner_.disconnect(); }
  bool isConnected() const override { return inner_.isConnected(); }
  bool publish(const std::string &topic, const std::string &payload,
              MqttQos qos, bool retain = false) override {
    ++publishCalls;
    return inner_.publish(topic, payload, qos, retain);
  }
  bool subscribe(const std::string &topic, MqttQos qos,
                 MqttMessageCallback callback) override {
    return inner_.subscribe(topic, qos, std::move(callback));
  }
  void poll() override { inner_.poll(); }
  int publishCalls = 0;

private:
  IDeviceTransport &inner_;
};

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

// Normal command, no failures: the first call publishes only ACCEPTED and
// defers the terminal; the next call (next loop iteration, with a
// transport.poll() in between in the real main loop) publishes only the
// terminal. Never both in the same call.
void test_valid_open_door_publishes_accepted_then_capability_disabled() {
  Fixture fixture;

  CommandPublishResult result = fixture.processor.processPayload(commandJson());

  TEST_ASSERT_TRUE(result.parsed);
  TEST_ASSERT_TRUE(result.acceptedPublished);
  TEST_ASSERT_FALSE(result.terminalPublished); // deferred to the next iteration
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_EQUAL(1, fixture.processor.pendingResponseCount());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));

  fixture.processor.processPending(); // next iteration publishes the terminal

  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "REJECTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
  TEST_ASSERT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "COMPLETED"));
  TEST_ASSERT_NULL(strstr(
      fixture.transport.publishedMessages()[1].payload.c_str(), "COMPLETED"));
  TEST_ASSERT_EQUAL(0, fixture.hardware.doorOutputCalls);
  TEST_ASSERT_EQUAL(0, fixture.systemControl.restartCount());
}

void test_terminal_diagnostic_reports_safe_capability_code_without_ids() {
  Fixture fixture;
  std::vector<std::string> terminalCodes;
  fixture.processor.setDiagnosticCallback(
      [&terminalCodes](const CommandDiagnostic &diagnostic) {
        if (diagnostic.stage == CommandDiagnosticStage::TerminalPublished) {
          terminalCodes.emplace_back(diagnostic.safeCode);
        }
      });

  fixture.processor.processPayload(commandJson());
  fixture.processor.processPending(); // drains the deferred terminal

  TEST_ASSERT_EQUAL(1, terminalCodes.size());
  TEST_ASSERT_EQUAL_STRING("CAPABILITY_DISABLED", terminalCodes[0].c_str());
  TEST_ASSERT_EQUAL(std::string::npos, terminalCodes[0].find(kDeviceId));
  TEST_ASSERT_EQUAL(std::string::npos, terminalCodes[0].find(kCommandId));
}

void test_duplicate_publishes_only_stored_terminal_response() {
  Fixture fixture;

  fixture.processor.processPayload(commandJson()); // publishes ACCEPTED, defers terminal
  fixture.processor.processPending(); // drains the first command's terminal
  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());

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

// Reproduces the field log where "ACCEPTED response publish failed" is
// immediately followed by "state online -> mqtt": the ACCEPTED publish
// itself fails. Both ACCEPTED and the already-computed terminal response
// must be queued (in order), and draining must publish at most one of them
// per processPending() call - never both in the same call, since each
// publish can itself block up to the configured transport timeout.
void test_accepted_publish_failure_queues_both_responses_one_publish_per_call() {
  Fixture fixture;
  fixture.transport.armPublishFailure(1);

  CommandPublishResult first = fixture.processor.processPayload(commandJson());
  TEST_ASSERT_FALSE(first.acceptedPublished);
  TEST_ASSERT_FALSE(first.terminalPublished);
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());

  // First call: only ACCEPTED goes out; terminal stays queued.
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_EQUAL(1, fixture.processor.pendingResponseCount());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));

  // Second call: only the terminal goes out.
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
}

// Reproduces the seq=3/4/7 field logs: ACCEPTED publishes fine, and the
// terminal - now always deferred to the next iteration rather than
// attempted immediately after ACCEPTED - fails on its own (first) drain
// attempt. Only the terminal is ever queued/retried; ACCEPTED is never
// resent. Also verifies the diagnostic callback reports the retried
// terminal's real device-side code (CAPABILITY_DISABLED), never a transport
// artifact and never nullptr.
void test_terminal_publish_failure_recovers_only_terminal() {
  Fixture fixture;
  // Call #1 = ACCEPTED (succeeds). Call #2 = the deferred terminal's first
  // drain attempt (fails).
  fixture.transport.armPublishFailureOnCall(2);
  std::vector<std::string> publishedTerminalCodes;
  fixture.processor.setDiagnosticCallback(
      [&publishedTerminalCodes](const CommandDiagnostic &diagnostic) {
        if (diagnostic.stage == CommandDiagnosticStage::TerminalPublished &&
            diagnostic.safeCode != nullptr) {
          publishedTerminalCodes.emplace_back(diagnostic.safeCode);
        }
      });

  CommandPublishResult first = fixture.processor.processPayload(commandJson());
  TEST_ASSERT_TRUE(first.acceptedPublished);
  TEST_ASSERT_FALSE(first.terminalPublished); // deferred, not attempted yet
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_EQUAL(1, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(0, publishedTerminalCodes.size());

  // First drain attempt hits the armed failure (call #2): terminal stays
  // queued, ACCEPTED is not touched again.
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(1, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(1, fixture.transport.publishedMessages().size());
  TEST_ASSERT_EQUAL(0, publishedTerminalCodes.size());

  // Second drain attempt succeeds (no more armed failures).
  fixture.processor.processPending();

  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[0].payload.c_str(),
             "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
  TEST_ASSERT_EQUAL(1, publishedTerminalCodes.size());
  TEST_ASSERT_EQUAL_STRING("CAPABILITY_DISABLED",
                           publishedTerminalCodes[0].c_str());
}

// Proves the exact bug from the field logs cannot recur: no RemoteCommandProcessor
// call path - a brand-new command in processPending()/processPayload(), or
// draining the outbox - ever issues more than one transport.publish() call.
// A CountingTransport wraps the real FakeDeviceTransport so a failure/success
// armed on the fake cannot mask a second, unwanted publish attempt.
void test_no_single_call_attempts_two_response_publishes() {
  ObservingHardware hardware;
  Intercom intercom(hardware);
  FakeClock clock;
  clock.setUnixTimeSeconds(1000);
  MemoryStore store;
  PersistentDedupCache cache(store);
  FakeSystemControl systemControl;
  CommandHandler handler(kDeviceId, clock, cache, intercom, systemControl);
  MqttTopics topics(devMqttTopicsConfig(kDeviceId));
  FakeDeviceTransport realTransport;
  realTransport.connect(kDeviceId);
  CountingTransport transport(realTransport);
  RemoteCommandProcessor processor(kDeviceId, transport, handler, topics);

  transport.publishCalls = 0;
  processor.processPayload(commandJson()); // new command: ACCEPTED only
  TEST_ASSERT_EQUAL(1, transport.publishCalls);

  transport.publishCalls = 0;
  processor.processPending(); // drains the deferred terminal: one attempt
  TEST_ASSERT_EQUAL(1, transport.publishCalls);

  transport.publishCalls = 0;
  processor.processPending(); // outbox already empty and no new command
  TEST_ASSERT_EQUAL(0, transport.publishCalls);
}

void test_reconnect_and_resubscribe_drains_outbox_in_order() {
  Fixture fixture;
  fixture.transport.armPublishFailure(1);
  fixture.processor.processPayload(commandJson());
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());

  // Simulate the transport actually dropping (as Esp32AwsIotTransport now
  // does on any publish failure) and the main loop tearing it down,
  // reconnecting, and resubscribing - draining must not require a new
  // incoming command to make progress.
  fixture.transport.disconnect();
  TEST_ASSERT_EQUAL(0, fixture.transport.subscriptionCount());
  fixture.transport.connect(kDeviceId);
  TEST_ASSERT_TRUE(fixture.processor.subscribe());

  // Two loop iterations, one publish attempt each, as the real main loop
  // would drive it via repeated processPending() calls.
  fixture.processor.processPending();
  fixture.processor.processPending();

  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
}

void test_outbox_retry_never_reexecutes_command_handler() {
  ObservingHardware hardware;
  Intercom intercom(hardware);
  FakeClock clock;
  clock.setUnixTimeSeconds(1000);
  InMemoryDedupCache innerCache;
  CountingDedupCache cache(innerCache);
  FakeSystemControl systemControl;
  CommandHandler handler(kDeviceId, clock, cache, intercom, systemControl);
  MqttTopics topics(devMqttTopicsConfig(kDeviceId));
  FakeDeviceTransport transport;
  transport.connect(kDeviceId);
  RemoteCommandProcessor processor(kDeviceId, transport, handler, topics);

  transport.armPublishFailure(1);
  processor.processPayload(commandJson());
  TEST_ASSERT_EQUAL(1, cache.findCalls);
  TEST_ASSERT_EQUAL(1, cache.recordCalls);
  TEST_ASSERT_EQUAL(2, processor.pendingResponseCount());

  processor.processPending();
  processor.processPending();

  TEST_ASSERT_EQUAL(0, processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, transport.publishedMessages().size());
  // Draining the outbox must never consult CommandHandler/the dedup cache
  // again - it only republishes already-computed response bytes.
  TEST_ASSERT_EQUAL(1, cache.findCalls);
  TEST_ASSERT_EQUAL(1, cache.recordCalls);
  TEST_ASSERT_EQUAL(0, hardware.doorOutputCalls);
}

void test_multiple_consecutive_commands_do_not_interleave_responses() {
  Fixture fixture;
  fixture.transport.armPublishFailure(1);
  CommandPublishResult firstResult =
      fixture.processor.processPayload(commandJson());
  TEST_ASSERT_FALSE(firstResult.acceptedPublished);
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());

  // A second, distinct command arrives through the normal callback path
  // while the first command's responses are still stuck in the outbox.
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  std::string secondId = kCommandId;
  secondId.back() = '0';
  fixture.transport.deliver(fixture.topics.commands(),
                            commandJsonWithId(secondId));

  // The armed failure is already consumed, so each call now succeeds, but
  // processPending() only publishes one outbox item per call - two calls
  // are needed to fully drain the first command's two queued responses.
  fixture.processor.processPending();
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));

  // Only now does the second, previously queued command get processed.
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(4, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[2].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[3].payload.c_str(),
             "CAPABILITY_DISABLED"));
}

void test_repeated_publish_failures_do_not_spin_in_a_single_call() {
  Fixture fixture;
  fixture.transport.armPublishFailure(1);
  fixture.processor.processPayload(commandJson());
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());

  // Keep every subsequent publish attempt failing too.
  fixture.transport.armPublishFailure(100);

  // A single processPending() call must attempt at most the front of the
  // outbox once and then return - it must never busy-loop retrying, and
  // leaves reconnect/backoff timing entirely to the caller (main loop).
  for (int i = 0; i < 3; ++i) {
    fixture.processor.processPending();
  }
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());
}

// kMaxOutboxSize (2) is a backstop for the processPending() invariant being
// bypassed, not working capacity for multiple commands. Trigger that
// unexpected state directly (two processPayload() calls while the first
// command's pair is still queued, skipping processPending()'s guard) and
// prove the outbox never evicts an already-pending entry: it stays exactly
// at the first command's ACCEPTED+terminal pair, bounded, with the second
// command's responses rejected and logged instead.
void test_outbox_never_evicts_a_pending_response_when_full() {
  Fixture fixture;
  fixture.transport.armPublishFailure(200);

  fixture.processor.processPayload(commandJson());
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());

  std::string secondId = kCommandId;
  secondId.back() = '0';
  // Bypasses processPending()'s "never start a new command while the outbox
  // is non-empty" guard on purpose, to exercise the capacity backstop.
  CommandPublishResult second =
      fixture.processor.processPayload(commandJsonWithId(secondId));

  TEST_ASSERT_FALSE(second.acceptedPublished);
  TEST_ASSERT_FALSE(second.terminalPublished);
  // Still exactly the first command's pair - nothing evicted, nothing from
  // the second command made it in, and the queue did not grow past the cap.
  TEST_ASSERT_EQUAL(2, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());

  bool sawCapacityError = false;
  for (const std::string &line : capturedLogs) {
    if (line.find("capacity") != std::string::npos) sawCapacityError = true;
    // Never log the command_id while reporting the overflow.
    TEST_ASSERT_EQUAL(std::string::npos, line.find(kCommandId));
    TEST_ASSERT_EQUAL(std::string::npos, line.find(secondId));
  }
  TEST_ASSERT_TRUE(sawCapacityError);

  // The first command's responses are still fully intact and drain in order
  // once the transport recovers - proving nothing was corrupted by the
  // rejected second command.
  fixture.transport.armPublishFailure(0);
  fixture.processor.processPending();
  fixture.processor.processPending();
  TEST_ASSERT_EQUAL(0, fixture.processor.pendingResponseCount());
  TEST_ASSERT_EQUAL(2, fixture.transport.publishedMessages().size());
  TEST_ASSERT_NOT_NULL(strstr(
      fixture.transport.publishedMessages()[0].payload.c_str(), "ACCEPTED"));
  TEST_ASSERT_NOT_NULL(
      strstr(fixture.transport.publishedMessages()[1].payload.c_str(),
             "CAPABILITY_DISABLED"));
}

void test_exact_topic_qos_callback_and_resubscription() {
  Fixture fixture;
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());

  fixture.transport.deliver(fixture.topics.commands(), commandJson());

  // Delivery is the MQTT callback context: publishing here would recursively
  // enter the same client and previously could wedge its TLS socket.
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());
  fixture.processor.processPending(); // publishes ACCEPTED, defers terminal
  fixture.processor.processPending(); // next iteration drains the terminal
  TEST_ASSERT_TRUE(fixture.processor.lastResult().terminalPublished);
  TEST_ASSERT_EQUAL_STRING(
      "interbridge/ib-0123456789abcdef0123456789abcdef/commands",
      fixture.topics.commands().c_str());
  TEST_ASSERT_EQUAL(
      static_cast<int>(MqttQos::AtLeastOnce),
      static_cast<int>(fixture.transport.publishedMessages()[0].qos));
  TEST_ASSERT_FALSE(fixture.transport.publishedMessages()[0].retain);
  TEST_ASSERT_EQUAL(static_cast<int>(MqttQos::AtLeastOnce),
                    static_cast<int>(fixture.transport.subscriptions()[0].qos));
  TEST_ASSERT_EQUAL(std::string::npos, fixture.topics.commands().find('#'));
  TEST_ASSERT_EQUAL(std::string::npos, fixture.topics.commands().find('+'));
  fixture.transport.disconnect();
  TEST_ASSERT_EQUAL(0, fixture.transport.subscriptionCount());
  fixture.transport.connect(kDeviceId);
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  TEST_ASSERT_EQUAL(1, fixture.transport.subscriptionCount());
}

void test_oversized_and_wrong_topic_messages_never_reach_processor() {
  Fixture fixture;
  TEST_ASSERT_TRUE(fixture.processor.subscribe());
  fixture.transport.deliver(fixture.topics.commands(),
                            std::string(kMaxJsonPayloadBytes + 1, 'x'));
  fixture.transport.deliver(
      "interbridge/ib-ffffffffffffffffffffffffffffffff/commands",
      commandJson());
  TEST_ASSERT_FALSE(fixture.processor.lastResult().parsed);
  TEST_ASSERT_EQUAL(0, fixture.transport.publishedMessages().size());
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
  RUN_TEST(test_terminal_diagnostic_reports_safe_capability_code_without_ids);
  RUN_TEST(test_duplicate_publishes_only_stored_terminal_response);
  RUN_TEST(test_deduplication_survives_handler_and_cache_reconstruction);
  RUN_TEST(test_strict_parser_rejects_invalid_contract_payloads);
  RUN_TEST(test_strict_parser_accepts_exactly_the_seven_contract_fields);
  RUN_TEST(test_strict_parser_rejects_legacy_payload_field);
  RUN_TEST(test_strict_parser_rejects_unknown_field);
  RUN_TEST(test_legacy_parser_preserves_payload_compatibility);
  RUN_TEST(test_physical_fields_are_rejected);
  RUN_TEST(test_publish_failures_are_observable);
  RUN_TEST(test_accepted_publish_failure_queues_both_responses_one_publish_per_call);
  RUN_TEST(test_terminal_publish_failure_recovers_only_terminal);
  RUN_TEST(test_no_single_call_attempts_two_response_publishes);
  RUN_TEST(test_reconnect_and_resubscribe_drains_outbox_in_order);
  RUN_TEST(test_outbox_retry_never_reexecutes_command_handler);
  RUN_TEST(test_multiple_consecutive_commands_do_not_interleave_responses);
  RUN_TEST(test_repeated_publish_failures_do_not_spin_in_a_single_call);
  RUN_TEST(test_outbox_never_evicts_a_pending_response_when_full);
  RUN_TEST(test_exact_topic_qos_callback_and_resubscription);
  RUN_TEST(test_oversized_and_wrong_topic_messages_never_reach_processor);
  RUN_TEST(test_logs_never_contain_raw_payload_or_identifiers);
  return UNITY_END();
}
