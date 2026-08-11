#pragma once

#include <cstdint>
#include <string>

namespace interbridge {

constexpr uint32_t kDefaultHealthIntervalSeconds = 3600;

struct ShadowReportedState {
    std::string firmwareVersion;
    std::string hardwareVersion;
    std::string intercomState;
    int wifiRssi = 0;
    uint64_t uptimeMs = 0;
    bool provisioned = false;
    uint32_t healthIntervalSeconds = kDefaultHealthIntervalSeconds;
};

// Result of applying an incoming Shadow delta: which supported fields
// changed, so the caller can apply them (e.g. update HealthReporter's
// interval) and persist/re-report as appropriate.
struct ShadowDelta {
    bool healthIntervalChanged = false;
    uint32_t healthIntervalSeconds = kDefaultHealthIntervalSeconds;
};

// Builds/parses payloads for the named AWS IoT Device Shadow
// ("interbridge"). See docs/communication-protocol.md > AWS IoT Device
// Shadow. Only health_interval_s is a supported desired field today;
// ring_timeout_ms/door_open_duration_ms/audio_volume are documented
// future values and intentionally not parsed yet - the architecture
// (delta parsing that ignores unknown fields) is already ready for them.
// Unknown fields in any Shadow payload are ignored, never rejected.
class DeviceShadow {
public:
    // Builds a "state.reported" update payload:
    // {"state":{"reported":{...}}}
    static std::string buildReportedUpdate(const ShadowReportedState& state);

    // Parses a /shadow/.../update/delta payload and extracts only the
    // fields this firmware currently understands. Malformed/empty
    // payloads or payloads with no supported fields yield a delta with
    // nothing changed - this must never crash on an unrecognized shape.
    static ShadowDelta parseDelta(const std::string& deltaJson);
};

} // namespace interbridge
