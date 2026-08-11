#include "provisioning_manager.h"

namespace {
constexpr const char* kWifiSsidKey = "wifi_ssid";
constexpr const char* kWifiPasswordKey = "wifi_password";
} // namespace

namespace interbridge {

ProvisioningManager::ProvisioningManager(IPersistentStore& store, IWifiConnection& wifi, IBleProvisioning& ble,
                                          std::string proofOfPossession)
    : store_(store),
      wifi_(wifi),
      ble_(ble),
      proofOfPossession_(std::move(proofOfPossession)),
      state_(ProvisioningState::Idle) {}

void ProvisioningManager::checkAtBoot() {
    if (!store_.has(kWifiSsidKey)) {
        enterProvisioning();
    }
}

void ProvisioningManager::requestProvisioning() {
    if (state_ == ProvisioningState::AwaitingCredentials || state_ == ProvisioningState::ConnectingWifi) {
        return; // already in progress
    }
    enterProvisioning();
}

void ProvisioningManager::enterProvisioning() {
    state_ = ProvisioningState::AwaitingCredentials;
    ble_.startAdvertising(proofOfPossession_);
    pendingEvent_ = ProtocolEventName::ProvisioningStarted;
}

void ProvisioningManager::update() {
    if (state_ == ProvisioningState::AwaitingCredentials) {
        auto credentials = ble_.pollReceivedCredentials();
        if (credentials.has_value()) {
            store_.set(kWifiSsidKey, credentials->ssid);
            store_.set(kWifiPasswordKey, credentials->password);
            ble_.stopAdvertising();

            WifiCredentials wifiCredentials{credentials->ssid.c_str(), credentials->password.c_str()};
            wifi_.begin(wifiCredentials);
            state_ = ProvisioningState::ConnectingWifi;
        }
        return;
    }

    if (state_ == ProvisioningState::ConnectingWifi) {
        wifi_.update();
        if (wifi_.isConnected()) {
            state_ = ProvisioningState::Completed;
            pendingEvent_ = ProtocolEventName::ProvisioningCompleted;
        }
        return;
    }
}

ProvisioningState ProvisioningManager::state() const {
    return state_;
}

std::optional<ProtocolEventName> ProvisioningManager::pollEvent() {
    auto event = pendingEvent_;
    pendingEvent_.reset();
    return event;
}

} // namespace interbridge
