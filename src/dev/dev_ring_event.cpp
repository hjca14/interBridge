#include "dev_ring_event.h"

#include <cstdio>
#include <ctime>
#include <utility>

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

DevRingEventCoordinator::DevRingEventCoordinator(DevRingButtonController& startButton,
                                                 DevRingButtonController& endButton, IRandomSource& random,
                                                 IEventOutbox& outbox, std::string deviceId, uint32_t timeoutMs)
    : startButton_(startButton),
      endButton_(endButton),
      random_(random),
      outbox_(outbox),
      deviceId_(std::move(deviceId)),
      timeoutMs_(timeoutMs),
      state_(DevCallState::Idle),
      activeCallId_(),
      callStartedAtMs_(0) {}

std::string DevRingEventCoordinator::enqueue(ProtocolEventName eventName, bool hasValidTime,
                                             int64_t unixTimeSeconds) {
    DeviceEvent event;
    event.deviceId = deviceId_;
    event.event = eventName;
    event.eventId = generateHexId(random_, "evt");
    event.callId = activeCallId_;
    event.timestamp = hasValidTime ? formatIso8601Utc(unixTimeSeconds) : "";
    outbox_.enqueue(event.eventId, event.toJson());
    return event.eventId;
}

DevCallSessionOutcome DevRingEventCoordinator::update(uint32_t nowMs, bool hasValidTime, int64_t unixTimeSeconds) {
    // Both buttons' debounce/lockout state must advance every call,
    // regardless of DevCallState - see the header comment on update().
    const bool startEdge = startButton_.update(nowMs);
    const bool endEdge = endButton_.update(nowMs);

    DevCallSessionOutcome outcome;

    if (startEdge) {
        if (state_ == DevCallState::Idle) {
            // A fresh call_id per session, generated before the event
            // itself so RING_DETECTED's own call_id is never empty. The
            // session only becomes Ringing once this enqueue has
            // happened - MemoryEventOutbox::enqueue() cannot itself fail
            // (bounded FIFO with eviction, not a fallible I/O call), so
            // "enqueued" and "session active" are inseparable here by
            // construction, matching the required failure policy.
            activeCallId_ = generateHexId(random_, "call");
            state_ = DevCallState::Ringing;
            callStartedAtMs_ = nowMs;
            outcome.kind = DevCallSessionEventKind::RingDetected;
            outcome.eventId = enqueue(ProtocolEventName::RingDetected, hasValidTime, unixTimeSeconds);
            outcome.callId = activeCallId_;
        } else {
            outcome.kind = DevCallSessionEventKind::StartIgnoredAlreadyRinging;
            outcome.callId = activeCallId_;
        }
        return outcome;
    }

    if (endEdge) {
        if (state_ == DevCallState::Ringing) {
            outcome.kind = DevCallSessionEventKind::RingEndedByButton;
            outcome.callId = activeCallId_;
            outcome.eventId = enqueue(ProtocolEventName::RingEnded, hasValidTime, unixTimeSeconds);
            state_ = DevCallState::Idle;
            activeCallId_.clear();
        } else {
            outcome.kind = DevCallSessionEventKind::EndIgnoredNoActiveCall;
        }
        return outcome;
    }

    if (state_ == DevCallState::Ringing && timeoutMs_ > 0 &&
        static_cast<uint32_t>(nowMs - callStartedAtMs_) >= timeoutMs_) {
        outcome.kind = DevCallSessionEventKind::RingEndedByTimeout;
        outcome.callId = activeCallId_;
        outcome.eventId = enqueue(ProtocolEventName::RingEnded, hasValidTime, unixTimeSeconds);
        state_ = DevCallState::Idle;
        activeCallId_.clear();
    }

    return outcome;
}

DevCallState DevRingEventCoordinator::state() const { return state_; }

const std::string& DevRingEventCoordinator::activeCallId() const { return activeCallId_; }

size_t publishPendingEvents(IEventOutbox& outbox, IDeviceTransport& transport, const std::string& topic) {
    size_t publishedCount = 0;
    for (const auto& entry : outbox.pending()) {
        if (!transport.publish(topic, entry.eventJson, MqttQos::AtLeastOnce)) {
            // Stop, don't skip: outbox.pending() is oldest-first, and a
            // later entry (e.g. a RING_ENDED) must never be published
            // ahead of an earlier one (e.g. its own RING_DETECTED) still
            // queued in front of it just because this attempt happened to
            // fail. The next call retries from the same front entry.
            break;
        }
        outbox.dequeue(entry.eventId);
        ++publishedCount;
    }
    return publishedCount;
}

} // namespace interbridge
