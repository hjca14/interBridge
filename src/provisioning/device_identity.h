#pragma once

#include <string>

#include "../core/random_id.h"
#include "../storage/persistent_store.h"

namespace interbridge {

struct DeviceIdentity {
    std::string deviceId;
    std::string hardwareVersion;
    std::string firmwareVersion;
    bool provisioned = false;
};

// Validates the production device_id format: "ib-" followed by exactly
// 32 lowercase hex characters (128 bits). See
// docs/communication-protocol.md > Device Identity.
bool isValidDeviceId(const std::string& deviceId);

// Loads a stable device_id from persistent storage, generating and
// persisting a new one only on first use (i.e. manufacturing/provisioning
// time - this is NOT a real manufacturing provisioning station, just the
// firmware-side "load or generate" logic). Never regenerates an
// already-stored id: device_id must stay stable across normal reboots.
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
