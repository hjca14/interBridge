#pragma once

#include <cstdint>
#include <string>

#include "dev_ring_button.h"
#include "../core/random_id.h"
#include "../network/mqtt_transport.h"
#include "../protocol/event_outbox.h"
#include "../protocol/messages.h"

namespace interbridge {

// Formats a Unix epoch-seconds value as a UTC ISO-8601 string
// ("2026-08-11T14:30:25Z"), matching DeviceEvent.timestamp's documented
// wire format (protocol/messages.h). Pure/native-testable - no Arduino
// dependency. Only meaningful when the caller already knows the clock is
// valid; callers must never call this otherwise.
std::string formatIso8601Utc(int64_t unixSeconds);

// DEV-only safety timeout: if a simulated call is never ended by a
// GPIO3 pulse, DevRingEventCoordinator ends it automatically after this
// many monotonic milliseconds, so the bench simulator can never remain
// stuck in RINGING forever. 60s, matching the app's own ring-timeout
// fallback (see docs/dev-ring-simulator.md > Call session state machine).
constexpr uint32_t kDevCallTimeoutMs = 60u * 1000u;

// A DEV-simulated call session's lifecycle. Deliberately minimal (two
// states) - this is a bench convenience, not core::StateMachine's real
// call-flow state machine, and never touches it.
enum class DevCallState { Idle, Ringing };

// What DevRingEventCoordinator::update() did on a given call, so the
// caller (dev_ring_simulator_main.cpp) can log precisely and never guess
// from side effects.
enum class DevCallSessionEventKind {
    None,                     // no qualifying edge/timeout this call
    RingDetected,              // GPIO4 valid press in Idle: new call_id, RING_DETECTED enqueued
    RingEndedByButton,         // GPIO3 valid press in Ringing: RING_ENDED enqueued
    RingEndedByTimeout,        // kDevCallTimeoutMs elapsed while Ringing: RING_ENDED enqueued
    StartIgnoredAlreadyRinging, // GPIO4 valid press while already Ringing: no-op
    EndIgnoredNoActiveCall,     // GPIO3 valid press while Idle: no-op
};

struct DevCallSessionOutcome {
    DevCallSessionEventKind kind = DevCallSessionEventKind::None;
    // Populated for RingDetected/RingEndedByButton/RingEndedByTimeout only
    // (the newly enqueued event's own event_id and the session's call_id);
    // empty for None/StartIgnoredAlreadyRinging/EndIgnoredNoActiveCall.
    std::string eventId;
    std::string callId;
};

// Ties two debounced physical button edges - "start" (GPIO4) and "end"
// (GPIO3) - plus an inactivity timeout, to a minimal two-state call
// session (DevCallState), and to the existing DeviceEvent/event-outbox
// contract already used in production (see
// main.cpp::publishProtocolEvent/updateNetwork()). Phase 3B.8's original
// version of this class only ever produced RING_DETECTED from a single
// button; this pass adds the matching RING_ENDED, correlated by a shared
// call_id, and never touches the real Si3050/RingDetector. Hardware-
// independent and natively testable - both buttons/clock/random-source/
// outbox are all injected. See docs/dev-ring-simulator.md > "Call session
// state machine" for the full state diagram and failure-mode discussion.
class DevRingEventCoordinator {
public:
    DevRingEventCoordinator(DevRingButtonController& startButton, DevRingButtonController& endButton,
                            IRandomSource& random, IEventOutbox& outbox, std::string deviceId,
                            uint32_t timeoutMs = kDevCallTimeoutMs);

    // Call every loop iteration with the current monotonic time and the
    // caller's own clock-validity/wall-time reading (mirrors
    // HealthReporter/ButtonController's "pass now explicitly" pattern -
    // this class does not need the full IClock interface). Both buttons'
    // debounce state is always advanced, every call, regardless of the
    // current DevCallState - only the *effect* of a qualifying edge
    // depends on state; debounce/lockout bookkeeping itself must never be
    // skipped, or a suppressed bounce could be misread as a fresh edge
    // once the state later changes.
    //
    // At most one outcome is ever produced per call (see class comment on
    // DevCallSessionEventKind): a start edge is checked before an end
    // edge, and the timeout check runs only if neither edge already
    // produced an outcome and the session is still Ringing after that -
    // so a GPIO3 press and an expiring timeout landing in the same call
    // can never both enqueue a RING_ENDED for the same session.
    DevCallSessionOutcome update(uint32_t nowMs, bool hasValidTime, int64_t unixTimeSeconds);

    DevCallState state() const;
    // "" when Idle; the active session's call_id while Ringing.
    const std::string& activeCallId() const;

private:
    std::string enqueue(ProtocolEventName eventName, bool hasValidTime, int64_t unixTimeSeconds);

    DevRingButtonController& startButton_;
    DevRingButtonController& endButton_;
    IRandomSource& random_;
    IEventOutbox& outbox_;
    std::string deviceId_;
    uint32_t timeoutMs_;

    DevCallState state_;
    std::string activeCallId_;
    uint32_t callStartedAtMs_;
};

// Drains the outbox against the given transport/topic, exactly mirroring
// main.cpp's updateNetwork() outbox loop: publish each pending entry
// as-is (never regenerating its JSON/event_id) and dequeue only on a
// successful publish, so an offline/failed attempt leaves the entry
// queued for the next call untouched. Stops at the first publish failure
// (rather than continuing to later entries) so a RING_ENDED can never be
// published ahead of the RING_DETECTED still queued in front of it for
// the same or an earlier call - the outbox is a FIFO and publishing must
// stay strictly in enqueue order. Returns the number of entries
// successfully published and dequeued this call.
size_t publishPendingEvents(IEventOutbox& outbox, IDeviceTransport& transport, const std::string& topic);

} // namespace interbridge
