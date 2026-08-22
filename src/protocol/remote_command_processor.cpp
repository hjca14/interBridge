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
    return result;
  }

  result.parsed = true;
  CommandResponses responses = handler_.handle(parsed.command);
  if (responses.hasAccepted) {
    result.acceptedPublished =
        transport_.publish(topics_.responsesIngest(),
                           responses.accepted.toJson(), MqttQos::AtLeastOnce);
    if (!result.acceptedPublished) {
      Logger::error("Remote command ACCEPTED response publish failed");
      return result;
    }
  }

  result.terminalPublished =
      transport_.publish(topics_.responsesIngest(), responses.terminal.toJson(),
                         MqttQos::AtLeastOnce);
  if (!result.terminalPublished) {
    Logger::error("Remote command terminal response publish failed");
  }
  return result;
}

const CommandPublishResult &RemoteCommandProcessor::lastResult() const {
  return lastResult_;
}

} // namespace interbridge
