#include "mqtt_smoke_handler.h"
#include "../protocol/command_handler.h"

namespace interbridge {
namespace {
ProtocolErrorCode parseError(CommandParseStatus status) {
    switch (status) {
        case CommandParseStatus::PayloadTooLarge: return ProtocolErrorCode::PayloadTooLarge;
        case CommandParseStatus::UnsupportedProtocolVersion: return ProtocolErrorCode::UnsupportedProtocolVersion;
        default: return ProtocolErrorCode::InvalidPayload;
    }
}
} // namespace

DevMqttSmokeHandler::DevMqttSmokeHandler(std::string deviceId, IClock& clock)
    : deviceId_(std::move(deviceId)), clock_(clock) {}

CommandResponse DevMqttSmokeHandler::reject(const DeviceCommand& command, ProtocolErrorCode code) const {
    CommandResponse response;
    response.deviceId = deviceId_;
    response.commandId = command.commandId;
    response.command = command.rawCommand;
    response.status = CommandStatus::Rejected;
    response.error = ProtocolError{code, defaultErrorMessage(code)};
    return response;
}

CommandResponse DevMqttSmokeHandler::handle(const std::string& payload) const {
    const CommandParseResult parsed = parseCommand(payload, deviceId_);
    if (parsed.status != CommandParseStatus::Ok) return reject(DeviceCommand{}, parseError(parsed.status));
    const DeviceCommand& command = parsed.command;
    if (!clock_.hasValidTime()) return reject(command, ProtocolErrorCode::ClockNotTrustworthy);
    if (!command.hasIssuedAt || !command.hasExpiresAt || command.expiresAtUnixSeconds <= command.issuedAtUnixSeconds ||
        command.issuedAtUnixSeconds > clock_.unixTimeSeconds() + kClockSkewToleranceSeconds) {
        return reject(command, ProtocolErrorCode::InvalidTimestamp);
    }
    if (clock_.unixTimeSeconds() > command.expiresAtUnixSeconds) return reject(command, ProtocolErrorCode::CommandExpired);
    return reject(command, ProtocolErrorCode::CommandNotAllowed);
}
} // namespace interbridge
