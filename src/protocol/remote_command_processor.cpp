#include "remote_command_processor.h"

#include "../core/logger.h"
#include "messages.h"

namespace interbridge {
namespace {
constexpr size_t kMaxPendingCommands = 4;
}

RemoteCommandProcessor::RemoteCommandProcessor(std::string deviceId,
                                               IDeviceTransport &transport,
                                               CommandHandler &handler,
                                               MqttTopics topics)
    : deviceId_(std::move(deviceId)), transport_(transport), handler_(handler),
      topics_(std::move(topics)) {}

bool RemoteCommandProcessor::enqueue(PendingResponse entry) {
  if (outbox_.size() >= kMaxOutboxSize) {
    // Never evict an already-pending response to make room for a new one -
    // dropping e.g. a still-queued ACCEPTED to fit a later terminal would
    // corrupt that command's ordering/semantics. Reaching capacity here
    // means the processPending() invariant (never start a new command while
    // the outbox is non-empty) was bypassed by a direct call or unexpected
    // state; reject and log rather than touching any existing entry or
    // taking any further action for this one.
    Logger::error(
        "Response outbox at capacity; response rejected without evicting a pending one");
    return false;
  }
  outbox_.push_back(std::move(entry));
  return true;
}

bool RemoteCommandProcessor::publishOrQueue(const std::string &topic,
                                            const std::string &payload,
                                            MqttQos qos, bool isTerminal,
                                            uint32_t commandSeq,
                                            const char *safeCode) {
  if (transport_.publish(topic, payload, qos))
    return true;
  enqueue({topic, payload, qos, isTerminal, commandSeq, safeCode});
  return false;
}

void RemoteCommandProcessor::drainOutbox() {
  if (outbox_.empty())
    return;
  const PendingResponse &front = outbox_.front();
  // Retrying a response never re-invokes CommandHandler or re-executes any
  // action - this republishes the exact bytes already computed, which the
  // backend must already treat idempotently for QoS 1 redelivery (see
  // docs/communication-protocol.md > Duplicate Command Protection > 20.1).
  // At most one publish attempt happens here (it can itself block up to the
  // configured transport timeout) - never a burst of retries in one call.
  if (!transport_.publish(front.topic, front.payload, front.qos))
    return; // Still unreachable - left queued; retry on a later call.
  if (diagnosticCallback_) {
    diagnosticCallback_({front.isTerminal
                             ? CommandDiagnosticStage::TerminalPublished
                             : CommandDiagnosticStage::AcceptedPublished,
                         front.safeCode, 0, 0, front.commandSeq});
  }
  outbox_.pop_front();
}

size_t RemoteCommandProcessor::pendingResponseCount() const {
  return outbox_.size();
}

bool RemoteCommandProcessor::subscribe() {
  return transport_.subscribe(
      topics_.commands(), MqttQos::AtLeastOnce,
      [this](const std::string &topic, const std::string &payload) {
        if (diagnosticCallback_) diagnosticCallback_({CommandDiagnosticStage::Received});
        if (topic != topics_.commands() ||
            payload.size() > kMaxJsonPayloadBytes) {
          Logger::warn("Remote command rejected before parsing");
          lastResult_ = CommandPublishResult{};
          return;
        }
        if (pendingPayloads_.size() >= kMaxPendingCommands) {
          Logger::warn("Remote command queue full; command dropped");
          return;
        }
        pendingPayloads_.push_back(payload);
      });
}

void RemoteCommandProcessor::processPending() {
  if (!outbox_.empty()) {
    // Older responses are still undelivered - never start a new command
    // until they are flushed, so ACCEPTED/terminal ordering per command_id
    // stays strict even across a disconnect/reconnect.
    drainOutbox();
    return;
  }
  if (pendingPayloads_.empty()) return;
  std::string payload = std::move(pendingPayloads_.front());
  pendingPayloads_.pop_front();
  lastResult_ = processPayload(payload);
}

CommandPublishResult
RemoteCommandProcessor::processPayload(const std::string &payload) {
  CommandPublishResult result;
  const uint32_t seq = ++commandSeq_;
  CommandParseResult parsed = parseCommand(payload, deviceId_);
  if (parsed.status != CommandParseStatus::Ok) {
    Logger::warn("Remote command rejected during validation");
    if (diagnosticCallback_) diagnosticCallback_({CommandDiagnosticStage::Rejected, "MESSAGE_INVALID", 0, 0, seq});
    return result;
  }

  result.parsed = true;
  // CommandHandler::handle() computes both ACCEPTED and the terminal result
  // synchronously, before ACCEPTED is even attempted below - safe today
  // only because DoorOpenCapability is Disabled and no physical action is
  // ever taken. This is NOT "ACCEPTED confirmed published, then execute
  // physically": a future capability that actually actuates hardware must
  // not reuse this synchronous shape as-is. It will need to split
  // validation, publishing (and confirming) ACCEPTED, and only then
  // triggering the physical action as a separate step gated on that
  // confirmation - this PR does not implement or guarantee that ordering.
  CommandResponses responses = handler_.handle(parsed.command);
  if (diagnosticCallback_) {
    if (responses.timeValidationPassed) {
      diagnosticCallback_({CommandDiagnosticStage::ValidationPassed, nullptr,
                           responses.ageSeconds, responses.remainingSeconds, seq});
    } else if (responses.terminal.error.has_value()) {
      diagnosticCallback_({CommandDiagnosticStage::Rejected,
                           toString(responses.terminal.error->code), 0, 0, seq});
    }
  }
  const char *terminalCode = responses.terminal.error.has_value()
                                 ? toString(responses.terminal.error->code)
                                 : toString(responses.terminal.status);
  const std::string terminalPayload = responses.terminal.toJson();

  if (responses.hasAccepted) {
    result.acceptedPublished = publishOrQueue(
        topics_.responsesIngest(), responses.accepted.toJson(),
        MqttQos::AtLeastOnce, /*isTerminal=*/false, seq, /*safeCode=*/nullptr);
    if (diagnosticCallback_) {
      diagnosticCallback_({result.acceptedPublished
                               ? CommandDiagnosticStage::AcceptedPublished
                               : CommandDiagnosticStage::AcceptedPending,
                           nullptr, 0, 0, seq});
    }
    if (!result.acceptedPublished) {
      // Fail closed for any future physical action: until ACCEPTED is
      // actually confirmed published, no action beyond what CommandHandler
      // already performed is taken. The terminal result is already known
      // (CommandHandler is never re-invoked), so queue it directly behind
      // ACCEPTED in the outbox rather than attempting to publish it now -
      // that would risk delivering it before ACCEPTED if the connection
      // recovers between the two publish attempts.
      enqueue({topics_.responsesIngest(), terminalPayload,
               MqttQos::AtLeastOnce, /*isTerminal=*/true, seq, terminalCode});
      if (diagnosticCallback_) {
        diagnosticCallback_({CommandDiagnosticStage::TerminalPending,
                             terminalCode, 0, 0, seq});
      }
      Logger::error(
          "Remote command ACCEPTED response publish failed; queued for retry");
      return result;
    }
  }

  result.terminalPublished = publishOrQueue(
      topics_.responsesIngest(), terminalPayload, MqttQos::AtLeastOnce,
      /*isTerminal=*/true, seq, terminalCode);
  if (!result.terminalPublished) {
    Logger::error(
        "Remote command terminal response publish failed; queued for retry");
  }
  if (diagnosticCallback_) {
    diagnosticCallback_({result.terminalPublished
                             ? CommandDiagnosticStage::TerminalPublished
                             : CommandDiagnosticStage::TerminalPending,
                         terminalCode, 0, 0, seq});
  }
  return result;
}

void RemoteCommandProcessor::setDiagnosticCallback(CommandDiagnosticCallback callback) {
  diagnosticCallback_ = std::move(callback);
}

const CommandPublishResult &RemoteCommandProcessor::lastResult() const {
  return lastResult_;
}

} // namespace interbridge
