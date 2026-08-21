#pragma once

#include <string>

#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "command_handler.h"

namespace interbridge {

struct CommandPublishResult {
  bool parsed = false;
  bool acceptedPublished = false;
  bool terminalPublished = false;
};

class RemoteCommandProcessor {
public:
  RemoteCommandProcessor(std::string deviceId, IDeviceTransport &transport,
                         CommandHandler &handler, MqttTopics topics);

  bool subscribe();
  CommandPublishResult processPayload(const std::string &payload);
  const CommandPublishResult &lastResult() const;

private:
  std::string deviceId_;
  IDeviceTransport &transport_;
  CommandHandler &handler_;
  MqttTopics topics_;
  CommandPublishResult lastResult_;
};

} // namespace interbridge
