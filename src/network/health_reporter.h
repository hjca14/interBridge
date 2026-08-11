#pragma once

#include <cstdint>

namespace interbridge {

constexpr uint32_t kDefaultHealthIntervalMs = 3600u * 1000u;

// Decides when a HealthReport should be published: on a fixed cadence
// (default 3600s, see docs/communication-protocol.md > Health Telemetry)
// or immediately when forceNextPublish() has been called. The actual
// detection of trigger conditions (boot, reconnect, firmware update,
// persistent hardware error, critically low heap, severe Wi-Fi
// degradation) lives in their respective modules, which call
// forceNextPublish() - this class only tracks cadence and the pending
// force flag, and is driven entirely by an explicit nowMs so it never
// needs to sleep in tests.
class HealthReporter {
public:
    explicit HealthReporter(uint32_t intervalMs = kDefaultHealthIntervalMs);

    // Call every main loop iteration with the current monotonic time.
    // Returns true (once) when a health report should be published now,
    // and records that time as the last publish time. Always due on the
    // very first call (boot).
    bool isDue(uint32_t nowMs);

    // Makes the very next isDue() call return true regardless of cadence.
    void forceNextPublish();

private:
    uint32_t intervalMs_;
    uint32_t lastPublishMs_;
    bool hasPublished_;
    bool forced_;
};

} // namespace interbridge
