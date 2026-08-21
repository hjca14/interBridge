#include "messages.h"

#define ARDUINOJSON_ENABLE_STD_STRING 1
#include <ArduinoJson.h>

namespace interbridge {

const char* toString(ProtocolEventName event) {
    switch (event) {
        case ProtocolEventName::RingDetected: return "RING_DETECTED";
        case ProtocolEventName::OffHook: return "OFF_HOOK";
        case ProtocolEventName::OnHook: return "ON_HOOK";
        case ProtocolEventName::CallStarted: return "CALL_STARTED";
        case ProtocolEventName::CallEnded: return "CALL_ENDED";
        case ProtocolEventName::DoorOpened: return "DOOR_OPENED";
        case ProtocolEventName::DoorOpenFailed: return "DOOR_OPEN_FAILED";
        case ProtocolEventName::ProvisioningStarted: return "PROVISIONING_STARTED";
        case ProtocolEventName::ProvisioningCompleted: return "PROVISIONING_COMPLETED";
        case ProtocolEventName::ProvisioningFailed: return "PROVISIONING_FAILED";
        case ProtocolEventName::FactoryResetRequested: return "FACTORY_RESET_REQUESTED";
        case ProtocolEventName::OtaStarted: return "OTA_STARTED";
        case ProtocolEventName::OtaCompleted: return "OTA_COMPLETED";
        case ProtocolEventName::OtaFailed: return "OTA_FAILED";
        case ProtocolEventName::Error: return "ERROR";
    }
    return "UNKNOWN_EVENT";
}

const char* toString(ProtocolIntercomState state) {
    switch (state) {
        case ProtocolIntercomState::Idle: return "IDLE";
        case ProtocolIntercomState::Ringing: return "RINGING";
        case ProtocolIntercomState::OffHook: return "OFF_HOOK";
        case ProtocolIntercomState::InCall: return "IN_CALL";
        case ProtocolIntercomState::Error: return "ERROR";
    }
    return "UNKNOWN_STATE";
}

bool isValidCommandId(const std::string& commandId) {
    if (commandId.size() != 32) {
        return false;
    }
    for (char c : commandId) {
        bool isLowercaseHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!isLowercaseHex) {
            return false;
        }
    }
    return true;
}

CommandType commandTypeFromString(const std::string& value) {
    if (value == "OPEN_DOOR") return CommandType::OpenDoor;
    if (value == "RESTART") return CommandType::Restart;
    if (value == "ENTER_PROVISIONING") return CommandType::EnterProvisioning;
    if (value == "FACTORY_RESET") return CommandType::FactoryReset;
    if (value == "ANSWER_CALL") return CommandType::AnswerCall;
    if (value == "REJECT_CALL") return CommandType::RejectCall;
    if (value == "END_CALL") return CommandType::EndCall;
    return CommandType::Unknown;
}

const char* toString(CommandStatus status) {
    switch (status) {
        case CommandStatus::Accepted: return "ACCEPTED";
        case CommandStatus::Completed: return "COMPLETED";
        case CommandStatus::Failed: return "FAILED";
        case CommandStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN_STATUS";
}

const char* toString(ProtocolErrorCode code) {
    switch (code) {
        case ProtocolErrorCode::InvalidPayload: return "INVALID_PAYLOAD";
        case ProtocolErrorCode::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
        case ProtocolErrorCode::UnsupportedProtocolVersion: return "UNSUPPORTED_PROTOCOL_VERSION";
        case ProtocolErrorCode::UnknownCommand: return "UNKNOWN_COMMAND";
        case ProtocolErrorCode::CommandNotAllowed: return "COMMAND_NOT_ALLOWED";
        case ProtocolErrorCode::CommandExpired: return "COMMAND_EXPIRED";
        case ProtocolErrorCode::ClockNotTrustworthy: return "CLOCK_NOT_TRUSTWORTHY";
        case ProtocolErrorCode::InvalidTimestamp: return "INVALID_TIMESTAMP";
        case ProtocolErrorCode::DeviceBusy: return "DEVICE_BUSY";
        case ProtocolErrorCode::NotProvisioned: return "NOT_PROVISIONED";
        case ProtocolErrorCode::WifiUnavailable: return "WIFI_UNAVAILABLE";
        case ProtocolErrorCode::CloudUnavailable: return "CLOUD_UNAVAILABLE";
        case ProtocolErrorCode::DoorOutputFailure: return "DOOR_OUTPUT_FAILURE";
        case ProtocolErrorCode::OtaDownloadFailed: return "OTA_DOWNLOAD_FAILED";
        case ProtocolErrorCode::OtaValidationFailed: return "OTA_VALIDATION_FAILED";
        case ProtocolErrorCode::OtaInstallFailed: return "OTA_INSTALL_FAILED";
        case ProtocolErrorCode::ProvisioningFailed: return "PROVISIONING_FAILED";
        case ProtocolErrorCode::InternalError: return "INTERNAL_ERROR";
        case ProtocolErrorCode::CapabilityDisabled: return "CAPABILITY_DISABLED";
    }
    return "UNKNOWN_ERROR";
}

std::string defaultErrorMessage(ProtocolErrorCode code) {
    switch (code) {
        case ProtocolErrorCode::InvalidPayload: return "Command payload is malformed or missing required fields";
        case ProtocolErrorCode::PayloadTooLarge: return "Command payload exceeds the maximum allowed size";
        case ProtocolErrorCode::UnsupportedProtocolVersion: return "Command uses an unsupported protocol version";
        case ProtocolErrorCode::UnknownCommand: return "Command type is not recognized";
        case ProtocolErrorCode::CommandNotAllowed: return "Command is not allowed in the current device state";
        case ProtocolErrorCode::CommandExpired: return "Command validity window has expired";
        case ProtocolErrorCode::ClockNotTrustworthy: return "Device clock is not yet synchronized; time-sensitive commands are refused";
        case ProtocolErrorCode::InvalidTimestamp: return "Command issued_at/expires_at is invalid or exceeds the allowed validity window";
        case ProtocolErrorCode::DeviceBusy: return "Device is busy and cannot execute the command right now";
        case ProtocolErrorCode::NotProvisioned: return "Device is not provisioned";
        case ProtocolErrorCode::WifiUnavailable: return "Wi-Fi is unavailable";
        case ProtocolErrorCode::CloudUnavailable: return "Cloud connection is unavailable";
        case ProtocolErrorCode::DoorOutputFailure: return "Door output could not be activated";
        case ProtocolErrorCode::OtaDownloadFailed: return "Firmware download failed";
        case ProtocolErrorCode::OtaValidationFailed: return "Firmware validation failed";
        case ProtocolErrorCode::OtaInstallFailed: return "Firmware install failed";
        case ProtocolErrorCode::ProvisioningFailed: return "Provisioning failed";
        case ProtocolErrorCode::InternalError: return "Internal firmware error";
        case ProtocolErrorCode::CapabilityDisabled: return "Door opening capability is disabled";
    }
    return "Unknown error";
}

std::string DeviceEvent::toJson() const {
    JsonDocument doc;
    doc["protocol_version"] = kProtocolVersion;
    doc["device_id"] = deviceId;
    doc["event"] = toString(event);
    if (!eventId.empty()) {
        doc["event_id"] = eventId;
    }
    if (!timestamp.empty()) {
        doc["timestamp"] = timestamp;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string HealthReport::toJson() const {
    JsonDocument doc;
    doc["protocol_version"] = kProtocolVersion;
    doc["device_id"] = deviceId;
    doc["firmware_version"] = firmwareVersion;
    doc["intercom_state"] = intercomState;
    doc["uptime_ms"] = uptimeMs;
    doc["wifi_rssi"] = wifiRssi;
    doc["free_heap"] = freeHeapBytes;
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string CommandResponse::toJson() const {
    JsonDocument doc;
    doc["protocol_version"] = kProtocolVersion;
    doc["device_id"] = deviceId;
    doc["command_id"] = commandId;
    doc["command"] = command;
    doc["status"] = toString(status);
    doc["issued_at"] = issuedAtUnixSeconds;
    doc["expires_at"] = expiresAtUnixSeconds;
    if (error.has_value()) {
        JsonObject errorObj = doc["error"].to<JsonObject>();
        errorObj["code"] = toString(error->code);
        errorObj["message"] = error->message;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

CommandParseResult parseCommand(const std::string& json, const std::string& expectedDeviceId) {
    CommandParseResult result;

    if (json.size() > kMaxJsonPayloadBytes) {
        result.status = CommandParseStatus::PayloadTooLarge;
        return result;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    if (!doc["protocol_version"].is<int>()) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }
    int version = doc["protocol_version"];
    if (version != kProtocolVersion) {
        result.status = CommandParseStatus::UnsupportedProtocolVersion;
        return result;
    }
    if (!doc["device_id"].is<const char*>() || doc["device_id"].as<std::string>() != expectedDeviceId) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    if (!doc["command_id"].is<const char*>()) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }
    std::string commandId = doc["command_id"].as<std::string>();
    if (!isValidCommandId(commandId)) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    if (!doc["command"].is<const char*>()) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }
    std::string rawCommand = doc["command"].as<std::string>();
    if (rawCommand.empty()) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    DeviceCommand cmd;
    cmd.type = commandTypeFromString(rawCommand);
    cmd.rawCommand = rawCommand;
    cmd.commandId = commandId;
    cmd.deviceId = doc["device_id"].as<std::string>();
    if (!doc["parameters"].is<JsonObjectConst>() || doc["parameters"].size() != 0 ||
        doc.containsKey("payload") || doc.containsKey("dtmf") || doc.containsKey("key") ||
        doc.containsKey("gpio") || doc.containsKey("pulse_duration") || doc.containsKey("mode")) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    if (doc["issued_at"].is<int64_t>()) {
        cmd.issuedAtUnixSeconds = doc["issued_at"].as<int64_t>();
        cmd.hasIssuedAt = true;
    }
    if (doc["expires_at"].is<int64_t>()) {
        cmd.expiresAtUnixSeconds = doc["expires_at"].as<int64_t>();
        cmd.hasExpiresAt = true;
    }
    if (!cmd.hasIssuedAt || !cmd.hasExpiresAt) {
        result.status = CommandParseStatus::InvalidPayload;
        return result;
    }

    result.status = CommandParseStatus::Ok;
    result.command = cmd;
    return result;
}

} // namespace interbridge
