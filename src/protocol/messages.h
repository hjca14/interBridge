#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace interbridge {

constexpr int kProtocolVersion = 1;

// Custom protocol JSON payloads (events/commands/responses/health) must
// not exceed this size. See docs/communication-protocol.md > MQTT v1
// Connection Profile.
constexpr size_t kMaxJsonPayloadBytes = 8192;

// ---- Event vocabulary ----
// See docs/communication-protocol.md > Event Publishing. Not every value
// has a real producer yet - see command_handler.cpp / provisioning /
// ota for what is actually wired up, and CONTEXT.md for the mapping from
// core::EventType.
enum class ProtocolEventName {
    RingDetected,
    OffHook,
    OnHook,
    CallStarted,
    CallEnded,
    DoorOpened,
    DoorOpenFailed,
    ProvisioningStarted,
    ProvisioningCompleted,
    ProvisioningFailed,
    FactoryResetRequested,
    OtaStarted,
    OtaCompleted,
    OtaFailed,
    Error,
};
const char* toString(ProtocolEventName event);

// ---- Commands ----
// Protocol v1 remotely supports only OpenDoor and Restart (see
// docs/communication-protocol.md > Remote Commands).
// EnterProvisioning/FactoryReset are recognized so a well-formed request
// gets a clear COMMAND_NOT_ALLOWED response instead of UNKNOWN_COMMAND,
// but are never executed remotely. AnswerCall/RejectCall/EndCall are
// reserved for future call/audio support.
enum class CommandType {
    OpenDoor,
    Restart,
    EnterProvisioning,
    FactoryReset,
    AnswerCall,
    RejectCall,
    EndCall,
    Unknown,
};
CommandType commandTypeFromString(const std::string& value);

enum class CommandStatus { Accepted, Completed, Failed, Rejected };
const char* toString(CommandStatus status);

enum class ProtocolErrorCode {
    InvalidPayload,
    PayloadTooLarge,
    UnsupportedProtocolVersion,
    UnknownCommand,
    CommandNotAllowed,
    CommandExpired,
    ClockNotTrustworthy,
    InvalidTimestamp,
    DeviceBusy,
    DoorOutputFailure,
    OtaDownloadFailed,
    OtaValidationFailed,
    OtaInstallFailed,
    ProvisioningFailed,
    InternalError,
};
const char* toString(ProtocolErrorCode code);

struct ProtocolError {
    ProtocolErrorCode code;
    std::string message;
};

// Stable, deterministic human-readable message for a given error code.
// Used both when an error is first raised and when a duplicate command
// response is reconstructed from the dedup cache (see command_cache.h),
// so a replayed response reads identically to the original.
std::string defaultErrorMessage(ProtocolErrorCode code);

// ---- Outbound: events ----

struct DeviceEvent {
    std::string deviceId;
    ProtocolEventName event;
    std::string eventId;
    std::string timestamp; // ISO-8601 UTC; empty if no valid wall-clock time yet
    std::string toJson() const;
};

// ---- Outbound: health (Basic Ingest, see docs > Health Telemetry) ----

struct HealthReport {
    std::string deviceId;
    std::string firmwareVersion;
    std::string intercomState;
    uint64_t uptimeMs = 0;
    int wifiRssi = 0;
    uint32_t freeHeapBytes = 0;
    std::string toJson() const;
};

// ---- Outbound: command responses ----

struct CommandResponse {
    std::string deviceId;
    std::string commandId;
    std::string command; // original command string, echoed back verbatim
    CommandStatus status;
    std::optional<ProtocolError> error;
    std::string toJson() const;
};

// ---- Inbound: commands ----

struct DeviceCommand {
    CommandType type;
    std::string rawCommand; // original "command" string as received
    std::string commandId;
    std::string rawPayload; // raw JSON text of the "payload" field, "" if absent

    // Command time-safety fields (see docs > Command Time Safety).
    // Unix epoch seconds. hasIssuedAt/hasExpiresAt false if the field was
    // absent or not a valid integer.
    int64_t issuedAtUnixSeconds = 0;
    bool hasIssuedAt = false;
    int64_t expiresAtUnixSeconds = 0;
    bool hasExpiresAt = false;
};

enum class CommandParseStatus {
    Ok,
    PayloadTooLarge,
    InvalidPayload,
    UnsupportedProtocolVersion,
};

struct CommandParseResult {
    CommandParseStatus status;
    DeviceCommand command; // only meaningful if status == Ok
};

// Parses+structurally validates a raw MQTT command payload: JSON
// validity, size limit, protocol_version, and presence of command/
// command_id. Does NOT validate command semantics (unknown command,
// expiry, allowed-in-current-state) - see command_handler.*.
CommandParseResult parseCommand(const std::string& json);

} // namespace interbridge
