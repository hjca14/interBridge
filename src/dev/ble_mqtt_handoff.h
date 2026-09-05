#pragma once

namespace interbridge {

// Governs the hand-off of Wi-Fi association ownership between the official
// BLE onboarding session (Esp32BleProvisioning/WiFiProv, which owns Wi-Fi
// for as long as one provisioning attempt is outstanding - including the
// "already provisioned, reconnecting directly without ever opening BLE"
// case, see docs/dev-ble-mqtt.md) and this environment's ordinary
// DevMqttSmokeState-driven connectivity cascade (Wi-Fi/NTP/MQTT/health).
//
// The connectivity cascade must never call WiFi.begin() - not even the
// no-argument "reconnect from stored config" form this environment uses,
// since it has no compile-time Wi-Fi credentials of its own - while BLE
// still owns the attempt: doing so could race the official wifi_provisioning
// manager's own esp_wifi_connect() call for the same interface. See
// shouldHandleConnectWifiAction()'s doc comment for the one action this
// actually gates.
//
// One-shot latch: BLE owns Wi-Fi from construction until the provisioning
// attempt concludes exactly once - forward that via markProvisioningEnded()
// from ARDUINO_EVENT_PROV_END, and defensively from this environment's own
// local BleOnboardingWindow bookkeeping (StartNotConfirmed/WindowTimedOut)
// in case the real event is ever missed. After that single hand-off,
// ownership never reverts to BLE for the remainder of this boot - a later
// Wi-Fi drop is always this environment's own connectivity cascade to
// recover (via WiFi.begin() reconnecting from whatever STA config is by
// then stored - originally BLE-provided, or from an even earlier boot),
// exactly like every other DEV bench entry point already does.
class BleMqttHandoffGate {
public:
    bool bleOwnsWifi() const { return bleOwnsWifi_; }

    // Idempotent - safe to call more than once (e.g. once from
    // ARDUINO_EVENT_PROV_END and again from local window bookkeeping that
    // fires around the same real event).
    void markProvisioningEnded() { bleOwnsWifi_ = false; }

    // True iff a DevSmokeAction::ConnectWifi should actually be acted on
    // (call the real WiFi.begin()) right now - i.e. connectivity, not BLE,
    // currently owns Wi-Fi. Every other DevSmokeAction (ResolveDns,
    // ConfigureTime, ConnectMqtt, RecoverWifi) is left ungated: those only
    // ever fire once DevMqttSmokeState has already observed a genuine
    // wifiConnected==true, at which point the Wi-Fi interface is already
    // associated (by BLE or otherwise) and using it for DNS/NTP/MQTT is not
    // a Wi-Fi *association* operation that could race the provisioning
    // manager.
    bool shouldHandleConnectWifiAction() const { return !bleOwnsWifi_; }

private:
    bool bleOwnsWifi_ = true;
};

} // namespace interbridge
