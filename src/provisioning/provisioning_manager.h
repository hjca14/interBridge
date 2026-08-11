#pragma once

#include <optional>
#include <string>

#include "../network/wifi.h"
#include "../protocol/messages.h"
#include "../storage/persistent_store.h"
#include "ble_provisioning.h"

namespace interbridge {

enum class ProvisioningState { Idle, AwaitingCredentials, ConnectingWifi, Completed, Failed };

// Converges every provisioning trigger (missing Wi-Fi config at boot, the
// physical button - see hardware/button.h) onto one coordinator, per
// docs/communication-protocol.md > Enter Provisioning ("these paths
// should converge on the same provisioning coordinator/service").
// ENTER_PROVISIONING is NOT remotely executable in protocol v1 (see
// protocol/command_handler.*), so MQTT is deliberately not one of the
// triggers here.
//
// Entering provisioning never erases stored device identity or performs
// a factory reset - see factory_reset_coordinator.h, which is a
// separate, explicitly-invoked class.
//
// Known limitation: ConnectingWifi has no timeout/retry-limit yet - if
// Wi-Fi never connects after credentials are received, the coordinator
// stays in ConnectingWifi indefinitely. See CONTEXT.md > Technical Debt.
class ProvisioningManager {
public:
    ProvisioningManager(IPersistentStore& store, IWifiConnection& wifi, IBleProvisioning& ble,
                         std::string proofOfPossession);

    // Call once at boot: enters provisioning automatically if no Wi-Fi
    // credentials are stored.
    void checkAtBoot();

    // Explicit trigger (physical button hold). No-op if already
    // provisioning.
    void requestProvisioning();

    // Call every main loop iteration.
    void update();

    ProvisioningState state() const;

    // Consumers (device_transport) poll for lifecycle events to publish,
    // since this class has no direct MQTT dependency.
    std::optional<ProtocolEventName> pollEvent();

private:
    void enterProvisioning();

    IPersistentStore& store_;
    IWifiConnection& wifi_;
    IBleProvisioning& ble_;
    std::string proofOfPossession_;
    ProvisioningState state_;
    std::optional<ProtocolEventName> pendingEvent_;
};

} // namespace interbridge
