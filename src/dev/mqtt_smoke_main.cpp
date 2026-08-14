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
#include "../network/mqtt_topics.h"

namespace {
using namespace interbridge;
constexpr uint16_t kMqttPort = 8883;
constexpr size_t kMqttBufferBytes = kMaxJsonPayloadBytes + 1024;
constexpr uint32_t kReconnectMaxMs = 300000;

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
uint32_t retryAt = 0;
uint32_t retryDelay = 1000;

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

void connectMqtt() {
    if (mqtt.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID).c_str())) {
        retryDelay = 1000;
        onConnected();
    } else {
        retryAt = millis() + retryDelay;
        retryDelay = min(retryDelay * 2, kReconnectMaxMs);
        safeStatus("MQTT connect", false);
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("[DEV MQTT] controlled smoke harness; physical actions disabled");
    Serial.println("[DEV MQTT] local Wi-Fi/TLS credentials configured (values not logged)");
    WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(250);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    tls.setCACert(INTERBRIDGE_DEV_ROOT_CA_PEM);
    tls.setCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    tls.setPrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    mqtt.begin(INTERBRIDGE_DEV_AWS_ENDPOINT, kMqttPort, tls);
    mqtt.setOptions(300, true, 1000); // keepalive, clean session, timeout
    mqtt.onMessage(onMessage);
    connectMqtt();
}

void loop() {
    mqtt.loop();
    if (!mqtt.connected() && static_cast<int32_t>(millis() - retryAt) >= 0) connectMqtt();
    delay(10);
}
