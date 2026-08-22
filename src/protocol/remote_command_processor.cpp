#include "remote_command_processor.h"

#include "../core/logger.h"
#include "messages.h"

namespace interbridge {

RemoteCommandProcessor::RemoteCommandProcessor(std::string deviceId,
                                               IDeviceTransport &transport,
                                               CommandHandler &handler,
                                               MqttTopics topics)
    : deviceId_(std::move(deviceId)), transport_(transport), handler_(handler),
      topics_(std::move(topics)) {}

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
        lastResult_ = processPayload(payload);
      });
}

CommandPublishResult
RemoteCommandProcessor::processPayload(const std::string &payload) {
  CommandPublishResult result;
  CommandParseResult parsed = parseCommand(payload, deviceId_);
  if (parsed.status != CommandParseStatus::Ok) {
    Logger::warn("Remote command rejected during validation");
    if (diagnosticCallback_) diagnosticCallback_({CommandDiagnosticStage::Rejected, "MESSAGE_INVALID"});
    return result;
  }

  result.parsed = true;
  CommandResponses responses = handler_.handle(parsed.command);
  if (diagnosticCallback_) {
    if (responses.timeValidationPassed) {
      diagnosticCallback_({CommandDiagnosticStage::ValidationPassed, nullptr,
                           responses.ageSeconds, responses.remainingSeconds});
    } else if (responses.terminal.error.has_value()) {
      diagnosticCallback_({CommandDiagnosticStage::Rejected,
                           toString(responses.terminal.error->code)});
    }
  }
  if (responses.hasAccepted) {
    result.acceptedPublished =
        transport_.publish(topics_.responsesIngest(),
                           responses.accepted.toJson(), MqttQos::AtLeastOnce);
    if (!result.acceptedPublished) {
      Logger::error("Remote command ACCEPTED response publish failed");
      return result;
    }
    if (diagnosticCallback_) diagnosticCallback_({CommandDiagnosticStage::AcceptedPublished});
  }

  result.terminalPublished =
      transport_.publish(topics_.responsesIngest(), responses.terminal.toJson(),
                         MqttQos::AtLeastOnce);
  if (!result.terminalPublished) {
    Logger::error("Remote command terminal response publish failed");
  }
  if (diagnosticCallback_) {
    const char *safeCode = "publish_failed";
    if (result.terminalPublished) {
      safeCode = responses.terminal.error.has_value()
                     ? toString(responses.terminal.error->code)
                     : toString(responses.terminal.status);
    }
    diagnosticCallback_({CommandDiagnosticStage::TerminalPublished, safeCode});
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
