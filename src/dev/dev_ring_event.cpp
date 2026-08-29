#include "dev_ring_event.h"

#include <cstdio>
#include <ctime>

#include "../protocol/messages.h"

namespace interbridge {

std::string formatIso8601Utc(int64_t unixSeconds) {
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tmValue{};
#if defined(_WIN32)
    gmtime_s(&tmValue, &t);
#else
    gmtime_r(&t, &tmValue);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmValue.tm_year + 1900, tmValue.tm_mon + 1,
                  tmValue.tm_mday, tmValue.tm_hour, tmValue.tm_min, tmValue.tm_sec);
    return std::string(buf);
}

DevRingEventCoordinator::DevRingEventCoordinator(DevRingButtonController& button, IRandomSource& random,
                                                 IEventOutbox& outbox, std::string deviceId)
    : button_(button), random_(random), outbox_(outbox), deviceId_(std::move(deviceId)) {}

std::string DevRingEventCoordinator::update(uint32_t nowMs, bool hasValidTime, int64_t unixTimeSeconds) {
    if (!button_.update(nowMs)) return "";

    DeviceEvent event;
    event.deviceId = deviceId_;
    event.event = ProtocolEventName::RingDetected;
    event.eventId = generateHexId(random_, "evt");
    event.timestamp = hasValidTime ? formatIso8601Utc(unixTimeSeconds) : "";
    outbox_.enqueue(event.eventId, event.toJson());
    return event.eventId;
}

size_t publishPendingEvents(IEventOutbox& outbox, IDeviceTransport& transport, const std::string& topic) {
    size_t publishedCount = 0;
    for (const auto& entry : outbox.pending()) {
        if (transport.publish(topic, entry.eventJson, MqttQos::AtLeastOnce)) {
            outbox.dequeue(entry.eventId);
            ++publishedCount;
        }
    }
    return publishedCount;
}

} // namespace interbridge
