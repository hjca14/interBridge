#if defined(INTERBRIDGE_DEV_BLE_PROVISIONING)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "../../include/interbridge_ble_dev_secrets.h"
#include "../provisioning/ble_provisioning.h"

namespace {
using namespace interbridge;

// Phase 3C.3: this isolated bench entry point now also receives real
// Wi-Fi credentials over the official ARDUINO_EVENT_PROV_CRED_* events -
// see WifiCredentialState's doc comment in ble_provisioning.h for why
// this firmware never buffers the SSID/password itself.
Esp32BleProvisioning provisioning(BleSecurityMode::Security1);
BleAdvertisementInfo currentAdvertisementInfo;

constexpr uint32_t kDevProvisioningWindowMs = 5u * 60u * 1000u;
// Short and explicit: how long to wait for ARDUINO_EVENT_PROV_START
// after requesting a start before treating it as a failed start rather
// than an open window.
constexpr uint32_t kProvStartConfirmationTimeoutMs = 10u * 1000u;
BleOnboardingWindow onboardingWindow(kProvStartConfirmationTimeoutMs, kDevProvisioningWindowMs);

// Deliberate pause so a bench USB-serial monitor has time to attach
// before the first boot line - a monitor that attaches even slightly
// late otherwise misses the one-time boot banner and start-request log.
constexpr uint32_t kSerialSettleDelayMs = 1000u;

BleAdvertisementInfo advertisementInfo() {
    const uint64_t mac = ESP.getEfuseMac();
    char id[36];
    snprintf(id, sizeof(id), "ib-%032llx", static_cast<unsigned long long>(mac));
    return buildBleAdvertisementInfo(id);
}

void onWifiEvent(arduino_event_t* event) {
    switch (event->event_id) {
        case ARDUINO_EVENT_PROV_INIT:
            Serial.println("[BLE] provisioning manager initialized");
            break;
        case ARDUINO_EVENT_PROV_START:
            // The only real evidence that BLE advertising is active -
            // see BleOnboardingWindow's doc comment. The device name is
            // not secret and is the exact identifier a bench tester
            // should look for in nRF Connect.
            provisioning.notifyAdvertisingStarted();
            onboardingWindow.confirmStart(millis());
            Serial.println("[BLE] onboarding service active");
            Serial.printf("[BLE] advertising as: %s\n", currentAdvertisementInfo.deviceName.c_str());
            break;
        case ARDUINO_EVENT_PROV_CRED_RECV:
            // Receiving this event means Protocomm Security 1 accepted the
            // encrypted request AND the official wifi_provisioning manager
            // already has the credentials and is applying them to the
            // Wi-Fi stack itself - this firmware never reads or copies
            // event->event_info.prov_cred_recv.ssid/.password (deliberately
            // not referenced below) and has no buffer of its own that could
            // leak them. See WifiCredentialState's doc comment.
            provisioning.notifySecureSessionEstablished();
            provisioning.notifyCredentialsReceived();
            Serial.println("[BLE] central connected");
            Serial.println("[BLE] secure session established (Security 1)");
            Serial.println("[BLE] Wi-Fi credentials received");
            Serial.println("[BLE] Wi-Fi connecting");
            break;
        case ARDUINO_EVENT_PROV_CRED_FAIL:
            // Rejected credentials do NOT end the BLE session or close the
            // window - the official manager keeps listening so the app can
            // retry with different credentials while the window is open.
            provisioning.notifyCredentialsRejected();
            Serial.printf("[BLE] failure: Wi-Fi credentials rejected (reason=%d)\n",
                          static_cast<int>(event->event_info.prov_fail_reason));
            break;
        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            // Wi-Fi connectivity only - no AWS IoT/Fleet Provisioning/
            // certificate/claim flow exists in this isolated image. See
            // docs/ble-onboarding.md's "Physical validation" section.
            provisioning.notifyWifiConnected();
            Serial.println("[BLE] Wi-Fi connected");
            break;
        case ARDUINO_EVENT_PROV_END:
            provisioning.notifyDisconnected();
            onboardingWindow.close();
            if (provisioning.wifiCredentialState() == WifiCredentialState::Connected) {
                Serial.println("[BLE] onboarding complete: Wi-Fi connected, provisioning service ended");
            } else {
                Serial.println("[BLE] disconnected");
            }
            break;
        default:
            break;
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    delay(kSerialSettleDelayMs);
    Serial.println();
    Serial.println("[BLE] InterBridge onboarding (isolated BLE bench build) booting");

    // Do NOT call Serial.setDebugOutput(true) or esp_log_level_set() with
    // a verbose level here. A temporary verbose ESP-IDF debug level was
    // used once to diagnose a real bench failure and, as a direct
    // consequence, exposed the DEV PoP through an upstream WiFiProv.cpp
    // log line - see docs/ble-onboarding.md's "Physical validation"
    // section. Only this file's own explicit, sanitized Serial.print*
    // calls may reach the serial port in this environment.
    WiFi.onEvent(onWifiEvent);

    currentAdvertisementInfo = advertisementInfo();
    Serial.println("[BLE] onboarding service start requested");
    Serial.println("[BLE] security mode: Protocomm Security 1 with PoP");

    // Arm the confirmation window BEFORE requesting start:
    // ARDUINO_EVENT_PROV_START can be dispatched (on the system event
    // task) essentially immediately once startAdvertising() calls into
    // WiFiProv.beginProvision() - possibly before control even returns
    // to this function. Arming requestStart() first guarantees
    // confirmStart() is never silently dropped as a no-op because the
    // window was not yet armed to receive it (see
    // BleOnboardingWindow::confirmStart()'s doc comment).
    onboardingWindow.requestStart(millis());
    if (!provisioning.startAdvertising(currentAdvertisementInfo, INTERBRIDGE_DEV_BLE_POP)) {
        // Local validation rejected the request before any real start was
        // even attempted - close the window just armed rather than
        // leaving a stale awaiting-confirmation state for a request that
        // never actually went out.
        onboardingWindow.close();
        Serial.println("[BLE] failure: onboarding service start request rejected");
        return;
    }
}

void loop() {
    switch (onboardingWindow.update(millis())) {
        case BleOnboardingWindowEvent::StartNotConfirmed:
            provisioning.stopAdvertising();
            Serial.println("[BLE] failure: onboarding service start not confirmed");
            break;
        case BleOnboardingWindowEvent::WindowTimedOut:
            provisioning.stopAdvertising();
            Serial.println("[BLE] onboarding window closed: timeout");
            break;
        case BleOnboardingWindowEvent::None:
            break;
    }
    delay(20);
}

#endif // INTERBRIDGE_DEV_BLE_PROVISIONING
