#pragma once

#include <cstdint>
#include <string>

#include "dev_ring_button.h"
#include "../core/random_id.h"
#include "../network/mqtt_transport.h"
#include "../protocol/event_outbox.h"

namespace interbridge {

// Formats a Unix epoch-seconds value as a UTC ISO-8601 string
// ("2026-08-11T14:30:25Z"), matching DeviceEvent.timestamp's documented
// wire format (protocol/messages.h). Pure/native-testable - no Arduino
// dependency. Only meaningful when the caller already knows the clock is
// valid; callers must never call this otherwise.
std::string formatIso8601Utc(int64_t unixSeconds);

// Ties a debounced physical button press to the existing DeviceEvent/
// event-outbox contract already used in production for RING_DETECTED
// (see main.cpp::publishProtocolEvent/updateNetwork()). Phase 3B.8
// bench-only: this coordinator only ever produces RING_DETECTED, never
// any other protocol event, and never touches the real Si3050/
// RingDetector. Hardware-independent and natively testable - the
// button/clock/random-source/outbox are all injected.
class DevRingEventCoordinator {
public:
    DevRingEventCoordinator(DevRingButtonController& button, IRandomSource& random, IEventOutbox& outbox,
                            std::string deviceId);

    // Call every loop iteration with the current monotonic time and the
    // caller's own clock-validity/wall-time reading (mirrors
    // HealthReporter/ButtonController's "pass now explicitly" pattern -
    // this class does not need the full IClock interface). Returns the
    // newly enqueued event_id on a valid press this call, or "" if no
    // press was detected. The same event_id is preserved verbatim across
    // any later retry, since retries only ever re-publish the already-
    // serialized entry already sitting in the outbox - see
    // publishPendingEvents() below.
    std::string update(uint32_t nowMs, bool hasValidTime, int64_t unixTimeSeconds);

private:
    DevRingButtonController& button_;
    IRandomSource& random_;
    IEventOutbox& outbox_;
    std::string deviceId_;
};

// Drains the outbox against the given transport/topic, exactly mirroring
// main.cpp's updateNetwork() outbox loop: publish each pending entry
// as-is (never regenerating its JSON/event_id) and dequeue only on a
// successful publish, so an offline/failed attempt leaves the entry
// queued for the next call untouched. Returns the number of entries
// successfully published and dequeued this call.
size_t publishPendingEvents(IEventOutbox& outbox, IDeviceTransport& transport, const std::string& topic);

} // namespace interbridge
