#pragma once

#include <cstddef>
#include <string>

#include "../hardware/clock.h"
#include "../intercom/intercom.h"
#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "../protocol/command_cache.h"
#include "../protocol/command_handler.h"
#include "../protocol/remote_command_processor.h"
#include "dev_disabled_hardware.h"

namespace interbridge {

// The one DEV command-processing composition shared by every DEV bench
// entry point (esp32-c3-dev-mqtt, esp32-c3-dev-ring-simulator). Before this
// existed, each *_main.cpp built its own copy of
// InMemoryDedupCache/DisabledHardware/Intercom/DisabledSystemControl/
// CommandHandler/RemoteCommandProcessor by hand - which is exactly how
// esp32-c3-dev-ring-simulator ended up silently missing the whole thing
// (see docs/dev-ring-simulator.md > "Command processing"). Owning that
// composition here, behind a small subscribe()/processPending() surface,
// makes that specific omission structurally impossible to repeat: an entry
// point either has a DevCommandEnvironment member and calls it, or it has
// no command processing at all - there is no partial/hand-copied middle
// ground left to drift out of sync.
//
// DoorOpenCapability stays Disabled (see command_handler.h) - no DEV entry
// point using this class can ever genuinely actuate a door or system
// action; a valid OPEN_DOOR only ever reaches ACCEPTED then
// REJECTED/CAPABILITY_DISABLED.
class DevCommandEnvironment {
public:
    DevCommandEnvironment(std::string deviceId, IClock &clock, IDeviceTransport &transport, MqttTopics topics);

    // Forwards to RemoteCommandProcessor::setDiagnosticCallback() - call once
    // from setup(), before subscribe() is ever attempted.
    void setDiagnosticCallback(CommandDiagnosticCallback callback);

    // Subscribes to the commands topic. Must be called again after every
    // reconnect - a previous subscription never survives one. See
    // RemoteCommandProcessor::subscribe().
    bool subscribe();

    // Drains at most one pending response and starts any newly delivered
    // command, once per call - see RemoteCommandProcessor::processPending().
    // Call this every loop iteration while the transport is connected.
    void processPending();

    size_t pendingResponseCount() const;

private:
    InMemoryDedupCache dedupCache_;
    DisabledHardware hardware_;
    Intercom intercom_;
    DisabledSystemControl systemControl_;
    CommandHandler handler_;
    RemoteCommandProcessor processor_;
};

} // namespace interbridge
