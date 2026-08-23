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

// Bounded, RAM-only outbox capacity. processPending() never starts a new
// queued command while the outbox is non-empty (see below), so under that
// invariant at most one command's own pair of responses (ACCEPTED +
// terminal) can ever be pending at once. kMaxOutboxSize exists as an
// explicit, logged backstop for that invariant being bypassed (e.g. a
// direct processPayload() call while responses are already queued) - not as
// working capacity for multiple commands' worth of delayed responses.
constexpr size_t kMaxOutboxSize = 2;

class RemoteCommandProcessor {
public:
  RemoteCommandProcessor(std::string deviceId, IDeviceTransport &transport,
                         CommandHandler &handler, MqttTopics topics);

  bool subscribe();
  // MQTT callbacks execute inside the client's poll() call.  Processing a
  // command there would publish recursively through the same client and can
  // deadlock its socket/TLS state. Drain commands from the main loop instead.
  //
  // If an earlier response is still stuck in the outbox (last publish
  // attempt failed), this attempts to publish only that one response and
  // returns - it never starts a new command until the outbox is empty. This
  // is what keeps ACCEPTED/terminal ordering strict across a reconnect, and
  // it bounds each call to at most one publish attempt (which can itself
  // block up to the configured transport timeout), never a burst. It never
  // re-invokes CommandHandler: outbox entries are already-serialized
  // response payloads.
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
    // The device's own sanitized terminal status/error code (e.g.
    // "CAPABILITY_DISABLED"), preserved so a retried terminal response still
    // reports its real code once published - never null for a terminal
    // entry, always null for an ACCEPTED entry (ACCEPTED carries no code).
    const char *safeCode;
  };

  // Appends to the outbox. Returns false without adding the entry if the
  // outbox is already at kMaxOutboxSize - it never evicts an existing
  // pending entry to make room (see the kMaxOutboxSize comment: reaching
  // capacity means the processPending() invariant was bypassed, which is
  // logged as an error rather than silently corrupting an older command's
  // already-queued response pair).
  bool enqueue(PendingResponse entry);
  // Attempts to publish immediately; on failure, appends to the outbox via
  // enqueue() so the response is retried after reconnect instead of being
  // silently dropped.
  bool publishOrQueue(const std::string &topic, const std::string &payload,
                      MqttQos qos, bool isTerminal, uint32_t commandSeq,
                      const char *safeCode);
  // Attempts to publish only the item at the front of the outbox, at most
  // once. On success, removes it. On failure, leaves it queued and returns -
  // Esp32AwsIotTransport::publish() already marks the session invalid on
  // failure, so the normal reconnect/backoff flow (owned by the caller,
  // e.g. main.cpp/mqtt_smoke_main.cpp) handles the retry on a later call.
  // Never attempts a second item in the same call.
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
