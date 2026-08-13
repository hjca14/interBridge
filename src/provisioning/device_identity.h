#pragma once

#include <string>

#include "../core/random_id.h"
#include "../storage/persistent_store.h"

namespace interbridge {

struct DeviceIdentity {
    std::string deviceId;    // technical identity: AWS ThingName, MQTT ClientId. Never typed by the user.
    std::string hardwareVersion;
    std::string firmwareVersion;
    // Human-facing onboarding identifier (12 decimal digits, e.g.
    // "482719362051"). Used ONLY as a fallback identity-resolution
    // method (QR/manual entry) so the app/backend can find this
    // physical unit when nearby BLE discovery isn't used - see
    // docs/communication-protocol.md > Onboarding. It is explicitly NOT
    // a private key, AWS credential, MQTT identity, or permanent
    // authorization token, and carries no cryptographic weight of its
    // own - see formatNumericCodeForDisplay() in core/random_id.h for
    // the human-readable "4827 1936 2051" grouping.
    std::string setupCode;
    bool provisioned = false;
};

// Validates the production device_id format: "ib-" followed by exactly
// 32 lowercase hex characters (128 bits). See
// docs/communication-protocol.md > Device Identity.
bool isValidDeviceId(const std::string& deviceId);

// Validates the setup_code format: exactly 12 decimal digits.
bool isValidSetupCode(const std::string& setupCode);

// Loads a stable device_id and setup_code from persistent storage,
// generating and persisting them only on first use (i.e. manufacturing/
// onboarding time - this is NOT a real manufacturing provisioning
// station, just the firmware-side "load or generate" logic). Neither
// value is ever regenerated once stored: both must stay stable across
// normal reboots, and setup_code must stay stable across re-provisioning
// (only a factory reset's manufacturing-identity boundary could change
// it, and factory reset explicitly preserves it - see
// factory_reset_coordinator.h).
class DeviceIdentityProvider {
public:
    DeviceIdentityProvider(IPersistentStore& store, IRandomSource& random,
                            std::string hardwareVersion, std::string firmwareVersion);

    // Returns the stable identity, generating and persisting one on
    // first call if none is stored yet.
    DeviceIdentity load();

    void setProvisioned(bool provisioned);

private:
    IPersistentStore& store_;
    IRandomSource& random_;
    std::string hardwareVersion_;
    std::string firmwareVersion_;
};

} // namespace interbridge
