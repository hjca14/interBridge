#include "factory_reset_coordinator.h"

namespace {
constexpr const char* kWifiSsidKey = "wifi_ssid";
constexpr const char* kWifiPasswordKey = "wifi_password";
constexpr const char* kProvisionedKey = "provisioned";
} // namespace

namespace interbridge {

FactoryResetCoordinator::FactoryResetCoordinator(IPersistentStore& store) : store_(store) {}

bool FactoryResetCoordinator::execute() {
    store_.remove(kWifiSsidKey);
    store_.remove(kWifiPasswordKey);
    store_.set(kProvisionedKey, "0");
    // Deliberately NOT removed: device_id, aws_certificate_pem,
    // aws_private_key_pem - see class-level comment.
    return true;
}

} // namespace interbridge
