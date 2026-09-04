#if defined(INTERBRIDGE_DEV_BLE_PROVISIONING)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "../../include/interbridge_ble_dev_secrets.h"
#include "../provisioning/ble_provisioning.h"

namespace {
using namespace interbridge;

// Real bench observation: the first physical attempt with this target
// printed "onboarding window closed: timeout" while nRF Connect never
// saw an "InterBridge-XXXX" device - the previous code treated the
// WiFiProv.beginProvision() call itself as proof advertising was active,
// so a service that never actually started still ran its full timer.
// BleOnboardingWindow and notifyAdvertisingStarted() close that gap: the
// window only opens once ARDUINO_EVENT_PROV_START is actually observed.
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
            // Receiving this event means Protocomm Security 1 has accepted
            // the encrypted request. Never log either credential field.
            provisioning.notifySecureSessionEstablished();
            provisioning.notifyCredentials(
                reinterpret_cast<const char*>(event->event_info.prov_cred_recv.ssid),
                reinterpret_cast<const char*>(event->event_info.prov_cred_recv.password));
            Serial.println("[BLE] central connected");
            Serial.println("[BLE] secure session established (Security 1)");
            break;
        case ARDUINO_EVENT_PROV_CRED_FAIL:
            provisioning.notifyFailure();
            Serial.println("[BLE] failure: provisioning credentials rejected");
            break;
        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            Serial.println("[BLE] provisioning request accepted");
            break;
        case ARDUINO_EVENT_PROV_END:
            provisioning.notifyDisconnected();
            onboardingWindow.close();
            Serial.println("[BLE] disconnected");
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
    Serial.println("[BLE] InterBridge onboarding (isolated Phase 3C.1 bench build) booting");

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
