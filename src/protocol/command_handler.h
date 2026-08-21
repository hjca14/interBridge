#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../hardware/clock.h"
#include "../hardware/system_control.h"
#include "../intercom/intercom.h"
#include "command_cache.h"
#include "messages.h"

namespace interbridge {

// Maximum command validity window (expires_at - issued_at), per
// docs/communication-protocol.md > Command Time Safety.
constexpr int64_t kOpenDoorMaxValiditySeconds = 10;
constexpr int64_t kRestartMaxValiditySeconds = 60;

// Tolerance for issued_at being slightly ahead of the device clock
// (clock drift), before it is treated as an invalid/suspicious timestamp.
constexpr int64_t kClockSkewToleranceSeconds = 5;

// Validates and dispatches already-parsed remote commands (see
// protocol/messages.h > parseCommand for the parsing step). Protocol v1
// remotely executes only OPEN_DOOR and RESTART.
// ENTER_PROVISIONING/FACTORY_RESET are recognized but always rejected -
// they can only be triggered locally (missing Wi-Fi config or the
// physical button). ANSWER_CALL/REJECT_CALL/END_CALL are reserved and
// always rejected. Anything else is UNKNOWN_COMMAND.
//
// Because the current door actuation (Intercom::requestDoorOpen) and
// restart (ISystemControl::restart) are synchronous, handle() returns a
// single terminal response (COMPLETED/FAILED/REJECTED) rather than an
// immediate ACCEPTED followed later by a separate terminal publish - see
// CONTEXT.md > Technical Debt for the two-phase lifecycle this should
// grow into once door actuation can be genuinely asynchronous.
enum class DoorCapability { Disabled, Dtmf, Relay };
struct DoorCapabilityConfig { DoorCapability capability = DoorCapability::Disabled; };

class CommandHandler {
public:
    CommandHandler(std::string deviceId, IClock& clock, IDedupCache& dedupCache, Intercom& intercom,
                    ISystemControl& systemControl);

    std::vector<CommandResponse> handle(const DeviceCommand& command);

private:
    CommandResponse buildResponse(const DeviceCommand& command, CommandStatus status,
                                   std::optional<ProtocolErrorCode> errorCode) const;
    CommandResponse fromCache(const DeviceCommand& command, const DedupEntry& entry) const;
    CommandResponse recordAndReturn(const DeviceCommand& command, CommandResponse response);

    // Returns the error to reject with, or std::nullopt if the command's
    // issued_at/expires_at pass time-safety validation.
    std::optional<ProtocolErrorCode> checkTimeSafety(const DeviceCommand& command,
                                                       int64_t maxValiditySeconds) const;

    std::string deviceId_;
    IClock& clock_;
    IDedupCache& dedupCache_;
    Intercom& intercom_;
    ISystemControl& systemControl_;
    DoorCapabilityConfig capability_{};
};

} // namespace interbridge
