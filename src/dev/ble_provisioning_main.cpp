#if defined(INTERBRIDGE_DEV_BLE_PROVISIONING)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "../../include/interbridge_ble_dev_secrets.h"
#include "../provisioning/ble_provisioning.h"

namespace {
using namespace interbridge;

Esp32BleProvisioning provisioning(BleSecurityMode::Security1);
uint32_t windowStartedAtMs = 0;
bool windowOpen = false;
constexpr uint32_t kDevProvisioningWindowMs = 5u * 60u * 1000u;

BleAdvertisementInfo advertisementInfo() {
    const uint64_t mac = ESP.getEfuseMac();
    char id[36];
    snprintf(id, sizeof(id), "ib-%032llx", static_cast<unsigned long long>(mac));
    return buildBleAdvertisementInfo(id);
}

void onWifiEvent(arduino_event_t* event) {
    switch (event->event_id) {
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
            windowOpen = false;
            Serial.println("[BLE] disconnected");
            break;
        default:
            break;
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    delay(250);
    WiFi.onEvent(onWifiEvent);

    const BleAdvertisementInfo info = advertisementInfo();
    Serial.println("[BLE] starting 5-minute onboarding window");
    Serial.printf("[BLE] advertised name: %s\n", info.deviceName.c_str());
    Serial.println("[BLE] security mode: Protocomm Security 1 with PoP");
    if (!provisioning.startAdvertising(info, INTERBRIDGE_DEV_BLE_POP)) {
        Serial.println("[BLE] failure: onboarding service could not start");
        return;
    }
    windowStartedAtMs = millis();
    windowOpen = true;
}

void loop() {
    if (windowOpen && millis() - windowStartedAtMs >= kDevProvisioningWindowMs) {
        provisioning.stopAdvertising();
        windowOpen = false;
        Serial.println("[BLE] onboarding window closed: timeout");
    }
    delay(20);
}

#endif // INTERBRIDGE_DEV_BLE_PROVISIONING
