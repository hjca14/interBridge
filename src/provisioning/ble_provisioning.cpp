#include "ble_provisioning.h"

#include <algorithm>
#include <cctype>

#if defined(ARDUINO) && defined(INTERBRIDGE_DEV_BLE_PROVISIONING)
#include <WiFiProv.h>
#include <wifi_provisioning/manager.h>
#endif

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
    : securityMode_(requestedMode), advertising_(false), sessionActive_(false) {}

bool Esp32BleProvisioning::startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) {
#if defined(ARDUINO) && defined(INTERBRIDGE_DEV_BLE_PROVISIONING)
    if (securityMode_ != BleSecurityMode::Security1 || info.deviceName.empty() || proofOfPossession.empty()) {
        return false;
    }
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
                            WIFI_PROV_SECURITY_1, proofOfPossession.c_str(),
                            info.deviceName.c_str());
    advertising_ = true;
    return true;
#else
    (void)info;
    (void)proofOfPossession;
    return false;
#endif
}

void Esp32BleProvisioning::stopAdvertising() {
#if defined(ARDUINO) && defined(INTERBRIDGE_DEV_BLE_PROVISIONING)
    // Arduino-ESP32 2.0.17 has no WiFiProv end method. This public ESP-IDF
    // manager API stops an active provisioning service before releasing the
    // manager and scheme resources. With WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
    // deinitialization also releases the Bluetooth controller memory.
    wifi_prov_mgr_deinit();
#endif
    advertising_ = false;
    sessionActive_ = false;
}

bool Esp32BleProvisioning::isAdvertising() const {
    return advertising_;
}

bool Esp32BleProvisioning::isSessionActive() const {
    return sessionActive_;
}

BleSecurityMode Esp32BleProvisioning::securityMode() const {
    return securityMode_;
}

std::optional<WifiCredentialsPayload> Esp32BleProvisioning::pollReceivedCredentials() {
    auto result = receivedCredentials_;
    receivedCredentials_.reset();
    return result;
}

void Esp32BleProvisioning::notifySecureSessionEstablished() { sessionActive_ = true; }
void Esp32BleProvisioning::notifyDisconnected() { sessionActive_ = false; }
void Esp32BleProvisioning::notifyCredentials(const std::string& ssid, const std::string& password) {
    sessionActive_ = true;
    receivedCredentials_ = WifiCredentialsPayload{ssid, password};
}
void Esp32BleProvisioning::notifyFailure() { sessionActive_ = false; }

FakeBleProvisioning::FakeBleProvisioning(BleSecurityMode mode)
    : securityMode_(mode), advertising_(false), sessionActive_(false), startResult_(true) {}

bool FakeBleProvisioning::startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) {
    advertising_ = startResult_;
    lastPop_ = proofOfPossession;
    lastInfo_ = info;
    return startResult_;
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

void FakeBleProvisioning::setStartResult(bool result) { startResult_ = result; }

const std::string& FakeBleProvisioning::lastProofOfPossession() const {
    return lastPop_;
}

const BleAdvertisementInfo& FakeBleProvisioning::lastAdvertisementInfo() const {
    return lastInfo_;
}

} // namespace interbridge
