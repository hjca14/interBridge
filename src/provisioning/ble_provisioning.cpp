#include "ble_provisioning.h"

#include <algorithm>
#include <cctype>

namespace interbridge {

BleAdvertisementInfo buildBleAdvertisementInfo(const std::string& deviceId) {
    size_t hintLength = std::min<size_t>(4, deviceId.size());
    std::string hint = deviceId.substr(deviceId.size() - hintLength, hintLength);
    std::transform(hint.begin(), hint.end(), hint.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    BleAdvertisementInfo info;
    info.deviceIdentityHint = hint;
    info.deviceName = "InterBridge-" + hint;
    info.provisioningAvailable = true;
    return info;
}

Esp32BleProvisioning::Esp32BleProvisioning(BleSecurityMode requestedMode)
    : securityMode_(requestedMode), advertising_(false) {}

bool Esp32BleProvisioning::startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) {
    (void)info;
    (void)proofOfPossession;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

void Esp32BleProvisioning::stopAdvertising() {
    advertising_ = false;
}

bool Esp32BleProvisioning::isAdvertising() const {
    return advertising_;
}

bool Esp32BleProvisioning::isSessionActive() const {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

BleSecurityMode Esp32BleProvisioning::securityMode() const {
    return securityMode_;
}

std::optional<WifiCredentialsPayload> Esp32BleProvisioning::pollReceivedCredentials() {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return std::nullopt;
}

FakeBleProvisioning::FakeBleProvisioning(BleSecurityMode mode)
    : securityMode_(mode), advertising_(false), sessionActive_(false) {}

bool FakeBleProvisioning::startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) {
    advertising_ = true;
    lastPop_ = proofOfPossession;
    lastInfo_ = info;
    return true;
}

void FakeBleProvisioning::stopAdvertising() {
    advertising_ = false;
}

bool FakeBleProvisioning::isAdvertising() const {
    return advertising_;
}

bool FakeBleProvisioning::isSessionActive() const {
    return sessionActive_;
}

BleSecurityMode FakeBleProvisioning::securityMode() const {
    return securityMode_;
}

std::optional<WifiCredentialsPayload> FakeBleProvisioning::pollReceivedCredentials() {
    auto result = queuedCredentials_;
    queuedCredentials_.reset();
    return result;
}

void FakeBleProvisioning::injectCredentials(const WifiCredentialsPayload& credentials) {
    queuedCredentials_ = credentials;
}

void FakeBleProvisioning::setSessionActive(bool active) {
    sessionActive_ = active;
}

const std::string& FakeBleProvisioning::lastProofOfPossession() const {
    return lastPop_;
}

const BleAdvertisementInfo& FakeBleProvisioning::lastAdvertisementInfo() const {
    return lastInfo_;
}

} // namespace interbridge
