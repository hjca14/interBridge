#include "device_identity.h"

namespace {
constexpr const char* kDeviceIdKey = "device_id";
constexpr const char* kProvisionedKey = "provisioned";

bool isLowercaseHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
} // namespace

namespace interbridge {

bool isValidDeviceId(const std::string& deviceId) {
    static constexpr size_t kExpectedLength = 3 + 32; // "ib-" + 32 hex chars
    if (deviceId.size() != kExpectedLength) {
        return false;
    }
    if (deviceId[0] != 'i' || deviceId[1] != 'b' || deviceId[2] != '-') {
        return false;
    }
    for (size_t i = 3; i < deviceId.size(); i++) {
        if (!isLowercaseHex(deviceId[i])) {
            return false;
        }
    }
    return true;
}

DeviceIdentityProvider::DeviceIdentityProvider(IPersistentStore& store, IRandomSource& random,
                                                std::string hardwareVersion, std::string firmwareVersion)
    : store_(store),
      random_(random),
      hardwareVersion_(std::move(hardwareVersion)),
      firmwareVersion_(std::move(firmwareVersion)) {}

DeviceIdentity DeviceIdentityProvider::load() {
    DeviceIdentity identity;
    identity.hardwareVersion = hardwareVersion_;
    identity.firmwareVersion = firmwareVersion_;

    auto stored = store_.get(kDeviceIdKey);
    if (stored.has_value() && isValidDeviceId(*stored)) {
        identity.deviceId = *stored;
    } else {
        identity.deviceId = generateHexId(random_, "ib");
        store_.set(kDeviceIdKey, identity.deviceId);
    }

    auto provisionedFlag = store_.get(kProvisionedKey);
    identity.provisioned = provisionedFlag.has_value() && *provisionedFlag == "1";

    return identity;
}

void DeviceIdentityProvider::setProvisioned(bool provisioned) {
    store_.set(kProvisionedKey, provisioned ? "1" : "0");
}

} // namespace interbridge
