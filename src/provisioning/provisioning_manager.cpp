#include "provisioning_manager.h"

namespace {
constexpr const char* kWifiSsidKey = "wifi_ssid";
constexpr const char* kWifiPasswordKey = "wifi_password";
constexpr const char* kProvisionedKey = "provisioned";
} // namespace

namespace interbridge {

const char* toString(ProvisioningState state) {
    switch (state) {
        case ProvisioningState::Idle: return "IDLE";
        case ProvisioningState::ProvisioningAvailable: return "PROVISIONING_AVAILABLE";
        case ProvisioningState::BleSessionActive: return "BLE_SESSION_ACTIVE";
        case ProvisioningState::ConnectingWifi: return "CONNECTING_WIFI";
        case ProvisioningState::FleetProvisioning: return "FLEET_PROVISIONING";
        case ProvisioningState::CloudConnecting: return "CLOUD_CONNECTING";
        case ProvisioningState::Provisioned: return "PROVISIONED";
        case ProvisioningState::ProvisioningFailed: return "PROVISIONING_FAILED";
        case ProvisioningState::NotProvisioned: return "NOT_PROVISIONED";
    }
    return "UNKNOWN_PROVISIONING_STATE";
}

ProvisioningManager::ProvisioningManager(IPersistentStore& store, IWifiConnection& wifi, IBleProvisioning& ble,
                                          DeviceCredentialStore& credentialStore,
                                          FleetProvisioningCoordinator& fleetProvisioning,
                                          IStatusIndicator& statusIndicator, std::string deviceId,
                                          std::string proofOfPossession, BleAdvertisementInfo advertisementInfo)
    : store_(store),
      wifi_(wifi),
      ble_(ble),
      credentialStore_(credentialStore),
      fleetProvisioning_(fleetProvisioning),
      statusIndicator_(statusIndicator),
      deviceId_(std::move(deviceId)),
      proofOfPossession_(std::move(proofOfPossession)),
      advertisementInfo_(std::move(advertisementInfo)),
      state_(ProvisioningState::Idle),
      provisioningStartedAtMs_(0),
      wasAlreadyProvisionedWhenEntered_(false) {}

void ProvisioningManager::checkAtBoot(uint32_t nowMs) {
    if (!store_.has(kWifiSsidKey)) {
        enterProvisioning(nowMs);
    }
}

void ProvisioningManager::requestProvisioning(uint32_t nowMs) {
    if (isActivelyProvisioning()) {
        return; // already in progress
    }
    enterProvisioning(nowMs);
}

void ProvisioningManager::enterProvisioning(uint32_t nowMs) {
    wasAlreadyProvisionedWhenEntered_ = store_.has(kWifiSsidKey);
    provisioningStartedAtMs_ = nowMs;
    state_ = ProvisioningState::ProvisioningAvailable;
    if (!ble_.startAdvertising(advertisementInfo_, proofOfPossession_)) {
        state_ = ProvisioningState::ProvisioningFailed;
        statusIndicator_.show(ProvisioningIndication::ProvisioningFailure);
        pendingEvent_ = ProtocolEventName::ProvisioningFailed;
        return;
    }
    statusIndicator_.show(ProvisioningIndication::ProvisioningAvailable);
    pendingEvent_ = ProtocolEventName::ProvisioningStarted;
}

bool ProvisioningManager::isActivelyProvisioning() const {
    switch (state_) {
        case ProvisioningState::ProvisioningAvailable:
        case ProvisioningState::BleSessionActive:
        case ProvisioningState::ConnectingWifi:
        case ProvisioningState::FleetProvisioning:
        case ProvisioningState::CloudConnecting:
            return true;
        case ProvisioningState::Idle:
        case ProvisioningState::Provisioned:
        case ProvisioningState::NotProvisioned:
        case ProvisioningState::ProvisioningFailed:
            return false;
    }
    return false;
}

void ProvisioningManager::update(uint32_t nowMs) {
    if (!isActivelyProvisioning()) {
        return;
    }

    if (nowMs - provisioningStartedAtMs_ >= kProvisioningWindowMs) {
        handleTimeout(nowMs);
        return;
    }

    switch (state_) {
        case ProvisioningState::ProvisioningAvailable:
            if (ble_.isSessionActive()) {
                state_ = ProvisioningState::BleSessionActive;
                ble_.stopAdvertising();
                statusIndicator_.show(ProvisioningIndication::AppConnected);
                return;
            }
            checkForCredentials();
            break;

        case ProvisioningState::BleSessionActive:
            checkForCredentials();
            if (state_ == ProvisioningState::BleSessionActive && !ble_.isSessionActive()) {
                // Session dropped before credentials arrived; resume
                // advertising and keep waiting within the same window.
                state_ = ProvisioningState::ProvisioningAvailable;
                ble_.startAdvertising(advertisementInfo_, proofOfPossession_);
                statusIndicator_.show(ProvisioningIndication::ProvisioningAvailable);
            }
            break;

        case ProvisioningState::ConnectingWifi:
            wifi_.update();
            if (wifi_.isConnected()) {
                advanceAfterWifiConnected(nowMs);
            }
            break;

        default:
            // FleetProvisioning/CloudConnecting are resolved synchronously
            // inside advanceAfterWifiConnected() - not polled here.
            break;
    }
}

void ProvisioningManager::checkForCredentials() {
    auto credentials = ble_.pollReceivedCredentials();
    if (!credentials.has_value()) {
        return;
    }

    store_.set(kWifiSsidKey, credentials->ssid);
    store_.set(kWifiPasswordKey, credentials->password);
    ble_.stopAdvertising();

    WifiCredentials wifiCredentials{credentials->ssid.c_str(), credentials->password.c_str()};
    wifi_.begin(wifiCredentials);
    state_ = ProvisioningState::ConnectingWifi;
}

void ProvisioningManager::advanceAfterWifiConnected(uint32_t nowMs) {
    if (credentialStore_.hasCertificate()) {
        // Already has a permanent AWS IoT certificate (e.g. re-provisioning
        // just the Wi-Fi network) - Fleet Provisioning is not needed again.
        state_ = ProvisioningState::CloudConnecting;
        finishProvisioning(true, nowMs);
        return;
    }

    state_ = ProvisioningState::FleetProvisioning;
    FleetProvisioningResult result = fleetProvisioning_.provision(deviceId_);
    if (result == FleetProvisioningResult::Success) {
        state_ = ProvisioningState::CloudConnecting;
        finishProvisioning(true, nowMs);
    } else {
        finishProvisioning(false, nowMs);
    }
}

void ProvisioningManager::finishProvisioning(bool success, uint32_t nowMs) {
    if (success) {
        state_ = ProvisioningState::Provisioned;
        store_.set(kProvisionedKey, "1");
        statusIndicator_.show(ProvisioningIndication::ProvisioningSuccess);
        pendingEvent_ = ProtocolEventName::ProvisioningCompleted;
        return;
    }

    state_ = ProvisioningState::ProvisioningFailed;
    statusIndicator_.show(ProvisioningIndication::ProvisioningFailure);
    pendingEvent_ = ProtocolEventName::ProvisioningFailed;

    if (nowMs - provisioningStartedAtMs_ < kProvisioningWindowMs) {
        // Recoverable: resume advertising within the same window so the
        // user can retry without a fresh button press or reboot - see
        // docs/communication-protocol.md > Provisioning Timeout/Recovery.
        state_ = ProvisioningState::ProvisioningAvailable;
        ble_.startAdvertising(advertisementInfo_, proofOfPossession_);
        statusIndicator_.show(ProvisioningIndication::ProvisioningAvailable);
    }
}

void ProvisioningManager::handleTimeout(uint32_t nowMs) {
    (void)nowMs;
    ble_.stopAdvertising();
    statusIndicator_.clear();
    pendingEvent_ = ProtocolEventName::ProvisioningFailed;
    state_ = wasAlreadyProvisionedWhenEntered_ ? ProvisioningState::Idle : ProvisioningState::NotProvisioned;
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
