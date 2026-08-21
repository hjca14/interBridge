#pragma once

#include <optional>
#include <string>

#include "../hardware/clock.h"
#include "../hardware/system_control.h"
#include "../intercom/intercom.h"
#include "command_cache.h"
#include "messages.h"

namespace interbridge {

// Maximum command validity window (expires_at - issued_at), per
// docs/communication-protocol.md > Command Time Safety.
constexpr int64_t kOpenDoorMaxValiditySeconds = 30;

// Tolerance for issued_at being slightly ahead of the device clock
// (clock drift), before it is treated as an invalid/suspicious timestamp.
constexpr int64_t kClockSkewToleranceSeconds = 5;

// Validates and dispatches already-parsed remote commands (see
// protocol/messages.h > parseCommand for the parsing step). The explicit
// Phase 2D remote allowlist contains only OPEN_DOOR.
// ENTER_PROVISIONING/FACTORY_RESET are recognized but always rejected -
// they can only be triggered locally (missing Wi-Fi config or the
// physical button). ANSWER_CALL/REJECT_CALL/END_CALL are reserved and
// always rejected. Anything else is UNKNOWN_COMMAND.
//
// Door capability defaults to Disabled. Dtmf and Relay are reserved for
// future implementations and never actuated here.
enum class DoorOpenCapability { Disabled, Dtmf, Relay };
constexpr DoorOpenCapability kDoorOpenCapability = DoorOpenCapability::Disabled;

struct CommandResponses {
    CommandResponse accepted;
    CommandResponse terminal;
    bool hasAccepted = false;
};

class CommandHandler {
public:
    CommandHandler(std::string deviceId, IClock& clock, IDedupCache& dedupCache, Intercom& intercom,
                    ISystemControl& systemControl);

    CommandResponses handle(const DeviceCommand& command);

private:
    CommandResponse buildResponse(const DeviceCommand& command, CommandStatus status,
                                   std::optional<ProtocolErrorCode> errorCode) const;
    CommandResponse fromCache(const DeviceCommand& command, const DedupEntry& entry) const;
    CommandResponse recordAndReturn(const DeviceCommand& command, CommandResponse response);
    CommandResponses terminalOnly(CommandResponse response) const;

    // Returns the error to reject with, or std::nullopt if the command's
    // issued_at/expires_at pass time-safety validation.
    std::optional<ProtocolErrorCode> checkTimeSafety(const DeviceCommand& command,
                                                       int64_t maxValiditySeconds) const;

    std::string deviceId_;
    IClock& clock_;
    IDedupCache& dedupCache_;
    Intercom& intercom_;
    ISystemControl& systemControl_;
};

} // namespace interbridge
