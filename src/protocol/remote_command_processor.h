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
  // ACCEPTED was actually attempted (immediately, in processPayload()) and
  // published.
  AcceptedPublished,
  // ACCEPTED was actually attempted and the publish itself failed; queued
  // for retry. Never a device-side command result - do not confuse with a
  // terminal status/error code.
  AcceptedPending,
  // An ACCEPTED entry drained from the outbox was actually attempted again
  // and failed; still queued for a later retry. (ACCEPTED itself is always
  // attempted immediately when a command first arrives - see
  // processPayload() - so this can only happen on a later drain retry.)
  AcceptedPublishFailed,
  // The terminal was actually attempted and published - either immediately
  // (a duplicate command replay with no ACCEPTED to defer behind) or later
  // via drainOutbox().
  TerminalPublished,
  // ACCEPTED just published successfully; the terminal is queued for the
  // next iteration and has NOT been attempted at all yet. Not a failure.
  TerminalDeferred,
  // ACCEPTED itself failed/is pending; the terminal is queued behind it and
  // has NOT been attempted at all yet. Not a failure by itself - the
  // AcceptedPending event alongside it already reports the real failure.
  TerminalQueuedBehindAccepted,
  // The terminal was actually attempted and the publish itself failed;
  // still queued - either immediately (a duplicate command replay with no
  // ACCEPTED to defer behind) or later via drainOutbox(). The transport
  // layer already logs the sanitized mqtt_err=N for the underlying
  // failure - this reports the RemoteCommandProcessor-level outcome.
  TerminalPublishFailed,
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

// Bounded, RAM-only outbox capacity. The terminal response is always
// deferred here (never published in the same call as ACCEPTED - see
// processPayload()), so the outbox is populated on essentially every
// command with ACCEPTED, not only on a publish failure. processPending()
// never starts a new queued command while the outbox is non-empty (see
// below), so under that invariant at most one command's own pair of
// responses (ACCEPTED + terminal) can ever be pending at once.
// kMaxOutboxSize exists as an explicit, logged backstop for that invariant
// being bypassed (e.g. a direct processPayload() call while responses are
// already queued) - not as working capacity for multiple commands' worth of
// delayed responses.
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
  // Every call here attempts at most one response publish, full stop -
  // whether that is starting a brand-new command (which itself only
  // attempts ACCEPTED, never the terminal too - see processPayload()) or
  // draining one item off the outbox. Two back-to-back QoS 1 publishes with
  // no transport.poll() in between (which happens naturally between separate
  // processPending() calls in the real main loop) is what caused the second
  // publish to intermittently fail even when the first one succeeded. If an
  // earlier response is still stuck in the outbox, this never starts a new
  // command until the outbox is empty - that is what keeps ACCEPTED/terminal
  // ordering strict across a reconnect. It never re-invokes CommandHandler:
  // outbox entries are already-serialized response payloads.
  void processPending();
  // Starts a new command. Attempts to publish ACCEPTED (if any) at most
  // once; the terminal response is always deferred to a later
  // processPending() call - even when ACCEPTED just published successfully
  // - never attempted within this same call. See processPending()'s
  // one-publish-per-call contract.
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
