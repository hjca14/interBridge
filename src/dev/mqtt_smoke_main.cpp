#ifndef INTERBRIDGE_DEV_MQTT_SMOKE
#error "mqtt_smoke_main.cpp is only for INTERBRIDGE_DEV_MQTT_SMOKE"
#endif
#if !__has_include("interbridge_dev_secrets.h")
#error "DEV MQTT smoke build requires ignored include/interbridge_dev_secrets.h; copy the example first"
#endif

#include <Arduino.h>
#include <MQTT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "interbridge_dev_secrets.h"
#include "mqtt_smoke_handler.h"
#include "mqtt_smoke_state.h"
#include "../network/mqtt_topics.h"

namespace {
using namespace interbridge;
constexpr uint16_t kMqttPort = 8883;
constexpr size_t kMqttBufferBytes = kMaxJsonPayloadBytes + 1024;
constexpr uint32_t kSerialWaitMs = 1500;
constexpr uint32_t kHeartbeatMs = 15000;

class NtpClock final : public IClock {
public:
    uint32_t monotonicMs() const override { return millis(); }
    bool hasValidTime() const override { return time(nullptr) >= 1700000000; }
    int64_t unixTimeSeconds() const override { return time(nullptr); }
};

WiFiClientSecure tls;
MQTTClient mqtt(kMqttBufferBytes);
NtpClock clockSource;
MqttTopics topics(devMqttTopicsConfig(INTERBRIDGE_DEV_DEVICE_ID));
DevMqttSmokeHandler handler(INTERBRIDGE_DEV_DEVICE_ID, clockSource);
DevMqttSmokeState connectivity;
uint32_t heartbeatAt = 0;
bool mqttConfigured = false;

void safeStatus(const char* operation, bool ok) {
    Serial.printf("[DEV MQTT] %s: %s\n", operation, ok ? "ok" : "failed");
}

void onMessage(String& topic, String& payload) {
    if (topic != topics.commands().c_str()) return;
    const CommandResponse response = handler.handle(std::string(payload.c_str(), payload.length()));
    safeStatus("response publish", mqtt.publish(topics.responsesIngest().c_str(), response.toJson().c_str(),
                                                kDevSmokeRetain, kDevSmokeResponsePublishQos));
}

void onConnected() {
    safeStatus("command QoS1 subscription",
               mqtt.subscribe(topics.commands().c_str(), kDevSmokeCommandSubscribeQos));
    HealthReport health{INTERBRIDGE_DEV_DEVICE_ID, "dev-mqtt-smoke", "IDLE", millis(), WiFi.RSSI(), ESP.getFreeHeap()};
    safeStatus("health QoS0 publish", mqtt.publish(topics.healthIngest().c_str(), health.toJson().c_str(),
                                                   kDevSmokeRetain, kDevSmokeHealthPublishQos));
}

const char* stateName(DevSmokeState state) {
    switch (state) {
        case DevSmokeState::WaitingForWifi: return "wifi";
        case DevSmokeState::WaitingForDns: return "dns";
        case DevSmokeState::WaitingForTime: return "time";
        case DevSmokeState::WaitingForMqtt: return "mqtt";
        case DevSmokeState::Online: return "online";
    }
    return "unknown";
}

void heartbeat(uint32_t now) {
    if (!DevMqttSmokeState::deadlineReached(now, heartbeatAt)) return;
    heartbeatAt = now + kHeartbeatMs;
    Serial.printf("[DEV MQTT] status=%s wifi=%s time=%s mqtt=%s uptime_s=%lu\n", stateName(connectivity.state()),
                  WiFi.status() == WL_CONNECTED ? "up" : "down", clockSource.hasValidTime() ? "valid" : "pending",
                  mqtt.connected() ? "up" : "down", static_cast<unsigned long>(now / 1000));
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + kSerialWaitMs;
    while (!Serial && !DevMqttSmokeState::deadlineReached(millis(), serialDeadline)) delay(10);
    Serial.println("[DEV MQTT] controlled smoke harness; physical actions disabled");
    Serial.println("[DEV MQTT] local Wi-Fi/TLS credentials configured (values not logged)");
    tls.setCACert(INTERBRIDGE_DEV_ROOT_CA_PEM);
    tls.setCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    tls.setPrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    mqtt.onMessage(onMessage);
}

void loop() {
    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (!wifiConnected && mqtt.connected()) mqtt.disconnect();

    switch (connectivity.update(now, wifiConnected, clockSource.hasValidTime(), mqtt.connected())) {
        case DevSmokeAction::ConnectWifi:
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.println("[DEV MQTT] Wi-Fi connect requested");
            break;
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const IPAddress dns = WiFi.dnsIP();
            const bool networkReady = dns != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            Serial.printf("[DEV MQTT] DNS: %s\n", resolvedOk ? "ready" : "pending");
            break;
        }
        case DevSmokeAction::ConfigureTime:
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            Serial.println("[DEV MQTT] time sync requested");
            break;
        case DevSmokeAction::ConnectMqtt: {
            if (!mqttConfigured) {
                mqtt.begin(INTERBRIDGE_DEV_AWS_ENDPOINT, kMqttPort, tls);
                mqtt.setOptions(300, true, 1000); // keepalive, clean session, timeout
                mqttConfigured = true;
            }
            const bool connected = mqtt.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID).c_str());
            connectivity.mqttResult(now, connected);
            safeStatus("MQTT connect", connected);
            if (connected) onConnected();
            break;
        }
        case DevSmokeAction::None: break;
    }
    if (mqtt.connected()) mqtt.loop();
    heartbeat(now);
    delay(10);
}
