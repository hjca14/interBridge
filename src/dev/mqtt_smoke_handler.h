#pragma once
#include <string>
#include "../hardware/clock.h"
#include "../protocol/messages.h"

namespace interbridge {
constexpr int kDevSmokeCommandSubscribeQos = 1;
constexpr int kDevSmokeHealthPublishQos = 0;
constexpr int kDevSmokeEventPublishQos = 1;
constexpr int kDevSmokeResponsePublishQos = 1;
constexpr bool kDevSmokeRetain = false;
// Deliberately has no hardware, provisioning, reset, or system-control
// dependency, so this path cannot actuate physical behavior.
class DevMqttSmokeHandler {
public:
    DevMqttSmokeHandler(std::string deviceId, IClock& clock);
    CommandResponse handle(const std::string& payload) const;
private:
    CommandResponse reject(const DeviceCommand& command, ProtocolErrorCode code) const;
    std::string deviceId_;
    IClock& clock_;
};
} // namespace interbridge
