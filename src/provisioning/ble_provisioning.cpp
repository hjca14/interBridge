#include "ble_provisioning.h"

#include <algorithm>
#include <cctype>

#if defined(ARDUINO) && defined(INTERBRIDGE_DEV_BLE_PROVISIONING)
#include <WiFiProv.h>
#include <wifi_provisioning/manager.h>
#endif

namespace interbridge {

namespace {
// Wrap-safe "has nowMs reached deadlineMs" check, same signed-subtraction
// technique as DevMqttSmokeState::deadlineReached() in
// src/dev/mqtt_smoke_state.cpp - duplicated locally rather than shared
// across those two independent modules.
bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}
} // namespace

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
    // beginProvision() has no synchronous success/failure return, so this
    // is only the start REQUEST. advertising_ stays false until a real
    // ARDUINO_EVENT_PROV_START is observed - see notifyAdvertisingStarted().
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
    return std::nullopt;
}

void Esp32BleProvisioning::notifyAdvertisingStarted() { advertising_ = true; }
void Esp32BleProvisioning::notifySecureSessionEstablished() { sessionActive_ = true; }
void Esp32BleProvisioning::notifyDisconnected() { sessionActive_ = false; }
void Esp32BleProvisioning::notifyCredentialsReceived() { credentialState_ = WifiCredentialState::Connecting; }
void Esp32BleProvisioning::notifyWifiConnected() { credentialState_ = WifiCredentialState::Connected; }
void Esp32BleProvisioning::notifyCredentialsRejected() { credentialState_ = WifiCredentialState::Rejected; }

WifiCredentialState Esp32BleProvisioning::wifiCredentialState() const { return credentialState_; }

BleOnboardingWindow::BleOnboardingWindow(uint32_t confirmationTimeoutMs, uint32_t windowMs)
    : confirmationTimeoutMs_(confirmationTimeoutMs), windowMs_(windowMs) {}

void BleOnboardingWindow::requestStart(uint32_t nowMs) {
    awaitingConfirmation_ = true;
    open_ = false;
    confirmationDeadlineMs_ = nowMs + confirmationTimeoutMs_;
}

void BleOnboardingWindow::confirmStart(uint32_t nowMs) {
    if (!awaitingConfirmation_) {
        return;
    }
    awaitingConfirmation_ = false;
    open_ = true;
    windowDeadlineMs_ = nowMs + windowMs_;
}

void BleOnboardingWindow::close() {
    awaitingConfirmation_ = false;
    open_ = false;
}

BleOnboardingWindowEvent BleOnboardingWindow::update(uint32_t nowMs) {
    if (awaitingConfirmation_ && deadlineReached(nowMs, confirmationDeadlineMs_)) {
        awaitingConfirmation_ = false;
        return BleOnboardingWindowEvent::StartNotConfirmed;
    }
    if (open_ && deadlineReached(nowMs, windowDeadlineMs_)) {
        open_ = false;
        return BleOnboardingWindowEvent::WindowTimedOut;
    }
    return BleOnboardingWindowEvent::None;
}

bool BleOnboardingWindow::isAwaitingConfirmation() const { return awaitingConfirmation_; }
bool BleOnboardingWindow::isOpen() const { return open_; }

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
