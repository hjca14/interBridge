#include "ble_provisioning.h"

namespace interbridge {

Esp32BleProvisioning::Esp32BleProvisioning() : advertising_(false) {}

bool Esp32BleProvisioning::startAdvertising(const std::string& proofOfPossession) {
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

std::optional<WifiCredentialsPayload> Esp32BleProvisioning::pollReceivedCredentials() {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return std::nullopt;
}

FakeBleProvisioning::FakeBleProvisioning() : advertising_(false) {}

bool FakeBleProvisioning::startAdvertising(const std::string& proofOfPossession) {
    advertising_ = true;
    lastPop_ = proofOfPossession;
    return true;
}

void FakeBleProvisioning::stopAdvertising() {
    advertising_ = false;
}

bool FakeBleProvisioning::isAdvertising() const {
    return advertising_;
}

std::optional<WifiCredentialsPayload> FakeBleProvisioning::pollReceivedCredentials() {
    auto result = queuedCredentials_;
    queuedCredentials_.reset();
    return result;
}

void FakeBleProvisioning::injectCredentials(const WifiCredentialsPayload& credentials) {
    queuedCredentials_ = credentials;
}

const std::string& FakeBleProvisioning::lastProofOfPossession() const {
    return lastPop_;
}

} // namespace interbridge
