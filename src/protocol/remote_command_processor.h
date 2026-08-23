#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <deque>

#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "command_handler.h"

namespace interbridge {

struct CommandPublishResult {
  bool parsed = false;
  bool acceptedPublished = false;
  bool terminalPublished = false;
};

enum class CommandDiagnosticStage {
  Received,
  ValidationPassed,
  Rejected,
  AcceptedPublished,
  // Publish failed; the response is queued in the outbox for retry after
  // reconnect. Never a device-side command result - do not confuse with a
  // terminal status/error code.
  AcceptedPending,
  TerminalPublished,
  // Same as AcceptedPending, but for the terminal response.
  TerminalPending,
};
struct CommandDiagnostic {
  CommandDiagnosticStage stage;
  const char *safeCode = nullptr;
  int64_t ageSeconds = 0;
  int64_t remainingSeconds = 0;
  // Local, per-process sequence number (never derived from command_id) used
  // only to correlate DEV log lines belonging to the same command.
  uint32_t commandSeq = 0;
};
using CommandDiagnosticCallback = std::function<void(const CommandDiagnostic &)>;

// Bounded, RAM-only outbox capacity: at most kMaxPendingCommands queued
// incoming commands, each producing at most two responses (ACCEPTED +
// terminal).
constexpr size_t kMaxOutboxSize = 8;

class RemoteCommandProcessor {
public:
  RemoteCommandProcessor(std::string deviceId, IDeviceTransport &transport,
                         CommandHandler &handler, MqttTopics topics);

  bool subscribe();
  // MQTT callbacks execute inside the client's poll() call.  Processing a
  // command there would publish recursively through the same client and can
  // deadlock its socket/TLS state. Drain commands from the main loop instead.
  //
  // If earlier responses are still stuck in the outbox (last publish
  // attempt failed), this drains them first and does not start a new
  // command until they are flushed - this is what keeps ACCEPTED/terminal
  // ordering strict across a reconnect. It never re-invokes CommandHandler:
  // outbox entries are already-serialized response payloads.
  void processPending();
  CommandPublishResult processPayload(const std::string &payload);
  const CommandPublishResult &lastResult() const;
  void setDiagnosticCallback(CommandDiagnosticCallback callback);

  // Number of responses (ACCEPTED and/or terminal) still waiting to be
  // published. RAM-only: lost on reboot, see mqtt_smoke_main.cpp/main.cpp
  // and the PR description for the accepted tradeoff.
  size_t pendingResponseCount() const;

private:
  struct PendingResponse {
    std::string topic;
    std::string payload;
    MqttQos qos;
    bool isTerminal;
    uint32_t commandSeq;
  };

  // Appends to the outbox, evicting the oldest entry first if already at
  // kMaxOutboxSize (explicit, logged, bounded - never an unbounded queue).
  void enqueue(PendingResponse entry);
  // Attempts to publish immediately; on failure, appends to the outbox via
  // enqueue() so the response is retried after reconnect instead of being
  // silently dropped.
  bool publishOrQueue(const std::string &topic, const std::string &payload,
                      MqttQos qos, bool isTerminal, uint32_t commandSeq);
  // Publishes outbox entries front-to-back, stopping at the first failure
  // (preserves per-command ordering and never busy-loops/re-attempts the
  // remainder within the same call).
  void drainOutbox();

  std::string deviceId_;
  IDeviceTransport &transport_;
  CommandHandler &handler_;
  MqttTopics topics_;
  CommandPublishResult lastResult_;
  std::deque<std::string> pendingPayloads_;
  std::deque<PendingResponse> outbox_;
  uint32_t commandSeq_ = 0;
  CommandDiagnosticCallback diagnosticCallback_;
};

} // namespace interbridge
