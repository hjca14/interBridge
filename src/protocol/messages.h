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

// ---- Intercom state vocabulary ----
// Canonical publishable values for "intercom_state" in HealthReport and
// the Device Shadow "reported" object - see
// docs/communication-protocol.md > Intercom State. This is a small,
// closed set specifically so nothing ever serializes an arbitrary
// string (e.g. core::State's diagnostic "BOOT", which has no protocol
// equivalent). OffHook is part of the canonical vocabulary but is NOT
// currently reachable: the firmware's core state machine transitions
// directly from Ringing to InCall on the OffHook event and has no
// resting "picked up but not yet in a call" state to report honestly -
// see main.cpp's toProtocolIntercomState() and CONTEXT.md > Decisions.
enum class ProtocolIntercomState { Idle, Ringing, OffHook, InCall, Error };
const char* toString(ProtocolIntercomState state);

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

// command_id is backend-generated (never by this firmware) and, per
// docs/communication-protocol.md > Common Message Fields, must be
// exactly 32 lowercase hexadecimal characters - a bare 128-bit
// identifier with NO semantic prefix (unlike event_id's "evt-" prefix
// or device_id's "ib-" prefix, both of which are firmware-generated via
// core/random_id.h and keep their prefixes).
bool isValidCommandId(const std::string& commandId);

enum class CommandStatus { Accepted, Completed, Failed, Rejected };
const char* toString(CommandStatus status);

// Canonical protocol v1 error codes - see
// docs/communication-protocol.md > Error Codes for the authoritative
// list, including which party (device/backend/application) originates
// each one. A code that doesn't make sense for the firmware to emit
// (e.g. NOT_PROVISIONED, WIFI_UNAVAILABLE, CLOUD_UNAVAILABLE) still lives
// in this shared enum so the wire contract is complete, but nothing in
// this codebase currently constructs a ProtocolError with it - see
// CONTEXT.md > Decisions before wiring one of those up, so it isn't used
// to fabricate device-side behavior it doesn't actually have.
enum class ProtocolErrorCode {
    InvalidPayload,              // device: malformed/missing-field payload
    PayloadTooLarge,             // device: payload exceeds kMaxJsonPayloadBytes
    UnsupportedProtocolVersion,  // device: protocol_version mismatch
    UnknownCommand,               // device: command string not recognized
    CommandNotAllowed,            // device: recognized but not remotely executable / not allowed in current state
    CommandExpired,                // device: now > expires_at
    ClockNotTrustworthy,           // device: no valid wall-clock time yet (NTP not implemented)
    InvalidTimestamp,              // device: issued_at/expires_at missing, malformed, or window too large
    DeviceBusy,                     // device: reserved, not currently produced
    NotProvisioned,                  // backend: reserved, not produced by firmware
    WifiUnavailable,                  // backend: reserved, not produced by firmware
    CloudUnavailable,                  // backend/application: reserved, not produced by firmware
    DoorOutputFailure,                  // device: hardware reported door actuation failure
    OtaDownloadFailed,                   // device: OTA download step failed
    OtaValidationFailed,                  // device: OTA hash/signature validation failed
    OtaInstallFailed,                      // device: OTA install/reboot step failed
    ProvisioningFailed,                     // device: reserved for a ProvisioningManager failure path (not implemented yet)
    InternalError,                           // device: catch-all
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
    std::string commandId;  // 32 lowercase hex chars, validated by parseCommand() via isValidCommandId()
    std::string rawPayload; // raw JSON text of the "payload" field, "" if absent

    // Command time-safety fields (see docs > Command Time Safety).
    // Unix epoch seconds (NOT ISO-8601 - see docs > Common Message
    // Fields for why command timestamps and event timestamps use
    // different representations). hasIssuedAt/hasExpiresAt are false if
    // the field was absent or not a JSON integer (including if it was
    // given as an ISO-8601 string - that is treated as absent, not
    // parsed).
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
