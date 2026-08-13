#pragma once

#include <optional>
#include <string>

#include "../hardware/status_indicator.h"
#include "../network/wifi.h"
#include "../protocol/messages.h"
#include "../storage/credential_store.h"
#include "../storage/persistent_store.h"
#include "ble_provisioning.h"
#include "fleet_provisioning.h"

namespace interbridge {

// Named onboarding states, per docs/communication-protocol.md >
// Provisioning State Machine. This is a dedicated coordinator, separate
// from core::StateMachine (the intercom call-flow state machine) -
// onboarding lifecycle and call lifecycle are different concerns with
// different failure/recovery semantics, and folding one into the other
// would force the call-flow machine to grow states it doesn't need for
// its own purpose.
enum class ProvisioningState {
    Idle,                  // provisioned and operating normally; not advertising
    ProvisioningAvailable, // advertising, waiting for a BLE session
    BleSessionActive,       // app connected over BLE, waiting for Wi-Fi credentials
    ConnectingWifi,          // credentials received, connecting to Wi-Fi
    FleetProvisioning,        // Wi-Fi connected, obtaining AWS IoT credentials via the CSR flow
    CloudConnecting,           // AWS IoT credentials available; handing off to the normal MQTT connect loop
    Provisioned,                // onboarding completed successfully
    ProvisioningFailed,          // a step failed - see pollEvent(); recovers to ProvisioningAvailable if still within the window
    NotProvisioned,               // never completed onboarding and not currently advertising (e.g. after a window timeout)
};
const char* toString(ProvisioningState state);

// Initial BLE provisioning window: how long the device advertises/waits
// for onboarding to complete before giving up and returning to normal
// operation. Named per the task that introduced it - see
// docs/communication-protocol.md > Provisioning Entry Behavior.
constexpr uint32_t kProvisioningWindowMs = 5u * 60u * 1000u; // 5 minutes

// Converges every provisioning trigger (missing Wi-Fi config at boot, the
// physical button - see hardware/button.h) onto one coordinator, per
// docs/communication-protocol.md > Enter Provisioning ("these paths
// should converge on the same provisioning coordinator/service").
// ENTER_PROVISIONING is NOT remotely executable in protocol v1 (see
// protocol/command_handler.*), so MQTT is deliberately not one of the
// triggers here. QR/manual setup_code entry are handled entirely by the
// app/backend to resolve which physical device to connect to - by the
// time this coordinator sees anything, the app has already opened a BLE
// session, so there is only one code path here regardless of how the app
// found the device (nearby discovery, QR, or manual entry).
//
// Entering provisioning never erases stored device identity or performs
// a factory reset - see factory_reset_coordinator.h, which is a
// separate, explicitly-invoked class.
//
// Known limitations (see CONTEXT.md > Technical Debt):
//   - ConnectingWifi has no dedicated failure/timeout detection; a stuck
//     Wi-Fi connection is only caught by the overall kProvisioningWindowMs
//     timeout, not a shorter Wi-Fi-specific one.
//   - CloudConnecting does not itself verify a successful MQTT connection
//     - that remains main.cpp's ordinary, already-existing reconnect
//     loop's job (see ReconnectManager). This coordinator's job ends once
//     Wi-Fi + an AWS IoT certificate are both in place; it does not
//     duplicate connection/backoff logic that already exists elsewhere.
class ProvisioningManager {
public:
    ProvisioningManager(IPersistentStore& store, IWifiConnection& wifi, IBleProvisioning& ble,
                         DeviceCredentialStore& credentialStore, FleetProvisioningCoordinator& fleetProvisioning,
                         IStatusIndicator& statusIndicator, std::string deviceId, std::string proofOfPossession,
                         BleAdvertisementInfo advertisementInfo);

    // Call once at boot: enters provisioning automatically if no Wi-Fi
    // credentials are stored.
    void checkAtBoot(uint32_t nowMs);

    // Explicit trigger (physical button hold). No-op if already
    // provisioning.
    void requestProvisioning(uint32_t nowMs);

    // Call every main loop iteration.
    void update(uint32_t nowMs);

    ProvisioningState state() const;

    // Consumers (main.cpp) poll for lifecycle events to publish, since
    // this class has no direct MQTT dependency.
    std::optional<ProtocolEventName> pollEvent();

private:
    void enterProvisioning(uint32_t nowMs);
    void checkForCredentials();
    void advanceAfterWifiConnected(uint32_t nowMs);
    void finishProvisioning(bool success, uint32_t nowMs);
    void handleTimeout(uint32_t nowMs);
    bool isActivelyProvisioning() const;

    IPersistentStore& store_;
    IWifiConnection& wifi_;
    IBleProvisioning& ble_;
    DeviceCredentialStore& credentialStore_;
    FleetProvisioningCoordinator& fleetProvisioning_;
    IStatusIndicator& statusIndicator_;
    std::string deviceId_;
    std::string proofOfPossession_;
    BleAdvertisementInfo advertisementInfo_;

    ProvisioningState state_;
    std::optional<ProtocolEventName> pendingEvent_;
    uint32_t provisioningStartedAtMs_;
    bool wasAlreadyProvisionedWhenEntered_;
};

} // namespace interbridge
