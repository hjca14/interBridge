#pragma once

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

enum class CommandDiagnosticStage { Received, ValidationPassed, Rejected, AcceptedPublished, TerminalPublished };
struct CommandDiagnostic {
  CommandDiagnosticStage stage;
  const char *safeCode = nullptr;
  int64_t ageSeconds = 0;
  int64_t remainingSeconds = 0;
};
using CommandDiagnosticCallback = std::function<void(const CommandDiagnostic &)>;

class RemoteCommandProcessor {
public:
  RemoteCommandProcessor(std::string deviceId, IDeviceTransport &transport,
                         CommandHandler &handler, MqttTopics topics);

  bool subscribe();
  // MQTT callbacks execute inside the client's poll() call.  Processing a
  // command there would publish recursively through the same client and can
  // deadlock its socket/TLS state. Drain commands from the main loop instead.
  void processPending();
  CommandPublishResult processPayload(const std::string &payload);
  const CommandPublishResult &lastResult() const;
  void setDiagnosticCallback(CommandDiagnosticCallback callback);

private:
  std::string deviceId_;
  IDeviceTransport &transport_;
  CommandHandler &handler_;
  MqttTopics topics_;
  CommandPublishResult lastResult_;
  std::deque<std::string> pendingPayloads_;
  CommandDiagnosticCallback diagnosticCallback_;
};

} // namespace interbridge
