#include "command_handler.h"

namespace interbridge {

CommandHandler::CommandHandler(std::string deviceId, IClock& clock, IDedupCache& dedupCache, Intercom& intercom,
                                ISystemControl& systemControl)
    : deviceId_(std::move(deviceId)),
      clock_(clock),
      dedupCache_(dedupCache),
      intercom_(intercom),
      systemControl_(systemControl) {}

CommandResponse CommandHandler::buildResponse(const DeviceCommand& command, CommandStatus status,
                                               std::optional<ProtocolErrorCode> errorCode) const {
    CommandResponse response;
    response.deviceId = deviceId_;
    response.commandId = command.commandId;
    response.command = command.rawCommand;
    response.status = status;
    if (errorCode.has_value()) {
        response.error = ProtocolError{*errorCode, defaultErrorMessage(*errorCode)};
    }
    return response;
}

CommandResponse CommandHandler::fromCache(const DeviceCommand& command, const DedupEntry& entry) const {
    std::optional<ProtocolErrorCode> errorCode;
    if (entry.hasError) {
        errorCode = entry.errorCode;
    }
    return buildResponse(command, entry.status, errorCode);
}

CommandResponse CommandHandler::recordAndReturn(const DeviceCommand& command, CommandResponse response) {
    DedupEntry entry;
    entry.status = response.status;
    entry.hasError = response.error.has_value();
    if (entry.hasError) {
        entry.errorCode = response.error->code;
    }
    dedupCache_.record(command.commandId, entry);
    return response;
}

std::optional<ProtocolErrorCode> CommandHandler::checkTimeSafety(const DeviceCommand& command,
                                                                    int64_t maxValiditySeconds) const {
    if (!clock_.hasValidTime()) {
        return ProtocolErrorCode::ClockNotTrustworthy;
    }
    if (!command.hasIssuedAt || !command.hasExpiresAt) {
        return ProtocolErrorCode::InvalidTimestamp;
    }
    if (command.expiresAtUnixSeconds <= command.issuedAtUnixSeconds) {
        return ProtocolErrorCode::InvalidTimestamp;
    }
    int64_t validityWindow = command.expiresAtUnixSeconds - command.issuedAtUnixSeconds;
    if (validityWindow > maxValiditySeconds) {
        return ProtocolErrorCode::InvalidTimestamp;
    }

    int64_t now = clock_.unixTimeSeconds();
    if (command.issuedAtUnixSeconds > now + kClockSkewToleranceSeconds) {
        return ProtocolErrorCode::InvalidTimestamp;
    }
    if (now > command.expiresAtUnixSeconds) {
        return ProtocolErrorCode::CommandExpired;
    }

    return std::nullopt;
}

CommandResponse CommandHandler::handle(const DeviceCommand& command) {
    auto cached = dedupCache_.find(command.commandId);
    if (cached.has_value()) {
        return fromCache(command, *cached);
    }

    switch (command.type) {
        case CommandType::OpenDoor: {
            auto timeError = checkTimeSafety(command, kOpenDoorMaxValiditySeconds);
            if (timeError.has_value()) {
                return recordAndReturn(command, buildResponse(command, CommandStatus::Rejected, timeError));
            }
            bool opened = intercom_.requestDoorOpen();
            if (opened) {
                return recordAndReturn(command, buildResponse(command, CommandStatus::Completed, std::nullopt));
            }
            return recordAndReturn(
                command, buildResponse(command, CommandStatus::Failed, ProtocolErrorCode::DoorOutputFailure));
        }

        case CommandType::Restart: {
            auto timeError = checkTimeSafety(command, kRestartMaxValiditySeconds);
            if (timeError.has_value()) {
                return recordAndReturn(command, buildResponse(command, CommandStatus::Rejected, timeError));
            }
            systemControl_.restart();
            return recordAndReturn(command, buildResponse(command, CommandStatus::Accepted, std::nullopt));
        }

        case CommandType::EnterProvisioning:
        case CommandType::FactoryReset:
        case CommandType::AnswerCall:
        case CommandType::RejectCall:
        case CommandType::EndCall:
            return recordAndReturn(
                command, buildResponse(command, CommandStatus::Rejected, ProtocolErrorCode::CommandNotAllowed));

        case CommandType::Unknown:
        default:
            return recordAndReturn(
                command, buildResponse(command, CommandStatus::Rejected, ProtocolErrorCode::UnknownCommand));
    }
}

} // namespace interbridge
