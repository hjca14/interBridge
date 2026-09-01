#include "dev_command_environment.h"

#include <utility>

namespace interbridge {

DevCommandEnvironment::DevCommandEnvironment(std::string deviceId, IClock &clock, IDeviceTransport &transport,
                                              MqttTopics topics)
    : intercom_(hardware_),
      handler_(deviceId, clock, dedupCache_, intercom_, systemControl_),
      processor_(std::move(deviceId), transport, handler_, std::move(topics)) {}

void DevCommandEnvironment::setDiagnosticCallback(CommandDiagnosticCallback callback) {
    processor_.setDiagnosticCallback(std::move(callback));
}

bool DevCommandEnvironment::subscribe() { return processor_.subscribe(); }

void DevCommandEnvironment::processPending() { processor_.processPending(); }

size_t DevCommandEnvironment::pendingResponseCount() const { return processor_.pendingResponseCount(); }

} // namespace interbridge
