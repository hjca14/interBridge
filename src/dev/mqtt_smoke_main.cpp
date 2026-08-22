#ifndef INTERBRIDGE_DEV_MQTT_SMOKE
#error "mqtt_smoke_main.cpp is only for INTERBRIDGE_DEV_MQTT_SMOKE"
#endif
#if !__has_include("interbridge_dev_secrets.h")
#error "DEV MQTT smoke build requires ignored include/interbridge_dev_secrets.h; copy the example first"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "interbridge_dev_secrets.h"
#include "mqtt_smoke_state.h"
#include "../hardware/clock.h"
#include "../hardware/gpio.h"
#include "../hardware/system_control.h"
#include "../intercom/intercom.h"
#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "../protocol/command_cache.h"
#include "../protocol/command_handler.h"
#include "../protocol/remote_command_processor.h"
#include "../storage/credential_store.h"
#include "../storage/memory_store.h"

namespace {
using namespace interbridge;
constexpr uint16_t kMqttPort = 8883;
constexpr uint16_t kMqttKeepAliveSeconds = 300;
constexpr uint16_t kMqttTimeoutMs = 1000;
constexpr uint32_t kSerialWaitMs = 1500;
constexpr uint32_t kHeartbeatMs = 15000;

class NtpClock final : public IClock {
public:
    uint32_t monotonicMs() const override { return millis(); }
    bool hasValidTime() const override { return time(nullptr) >= 1700000000; }
    int64_t unixTimeSeconds() const override { return time(nullptr); }
};
class DisabledHardware final : public IHardwareIO {
public:
    bool readLineState() override { return false; }
    bool setDoorOutput(bool) override { return false; }
};
class DisabledSystemControl final : public ISystemControl {
public:
    void restart() override {}
};

AwsIotConnectionConfig connectionConfig() {
    AwsIotConnectionConfig config;
    config.endpoint = INTERBRIDGE_DEV_AWS_ENDPOINT;
    config.rootCaPem = INTERBRIDGE_DEV_ROOT_CA_PEM;
    config.port = kMqttPort;
    config.keepAliveSeconds = kMqttKeepAliveSeconds;
    config.timeoutMs = kMqttTimeoutMs;
    return config;
}

NtpClock clockSource;
// DEV-only transient composition: credentials and deduplication do not survive
// reboot. Production NVS and provisioning remain separate and pending.
MemoryStore devStore;
DeviceCredentialStore credentials(devStore);
InMemoryDedupCache dedupCache;
DisabledHardware hardware;
Intercom intercom(hardware);
DisabledSystemControl systemControl;
MqttTopics topics(devMqttTopicsConfig(INTERBRIDGE_DEV_DEVICE_ID));
Esp32AwsIotTransport transport(connectionConfig(), credentials);
CommandHandler commandHandler(INTERBRIDGE_DEV_DEVICE_ID, clockSource, dedupCache,
                              intercom, systemControl);
RemoteCommandProcessor processor(INTERBRIDGE_DEV_DEVICE_ID, transport,
                                 commandHandler, topics);
DevMqttSmokeState connectivity;
uint32_t heartbeatAt = 0;
bool subscribed = false;

void safeStatus(const char* operation, bool ok) {
    Serial.printf("[DEV MQTT] %s: %s\n", operation, ok ? "ok" : "failed");
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
    Serial.printf("[DEV MQTT] status=%s wifi=%s time=%s mqtt=%s uptime_s=%lu\n",
                  stateName(connectivity.state()), WiFi.status() == WL_CONNECTED ? "up" : "down",
                  clockSource.hasValidTime() ? "valid" : "pending",
                  transport.isConnected() ? "up" : "down",
                  static_cast<unsigned long>(now / 1000));
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + kSerialWaitMs;
    while (!Serial && !DevMqttSmokeState::deadlineReached(millis(), serialDeadline)) delay(10);
    Serial.println("[DEV MQTT] production-path harness; physical actions disabled");
    Serial.println("[DEV MQTT] local configuration loaded into transient DEV memory (values not logged)");
    credentials.saveCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    credentials.savePrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
}

void loop() {
    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if ((!wifiConnected || !clockSource.hasValidTime()) && transport.isConnected()) {
        transport.disconnect();
        subscribed = false;
    }
    if (!transport.isConnected()) subscribed = false;

    switch (connectivity.update(now, wifiConnected, clockSource.hasValidTime(), transport.isConnected())) {
        case DevSmokeAction::ConnectWifi:
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.println("[DEV MQTT] Wi-Fi connect requested");
            break;
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
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
            const bool connected = clockSource.hasValidTime() &&
                transport.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID));
            if (connected) {
                subscribed = processor.subscribe();
                safeStatus("command QoS1 subscription", subscribed);
                if (!subscribed) transport.disconnect();
            }
            connectivity.mqttResult(now, connected && subscribed);
            safeStatus("MQTT connect", connected && subscribed);
            break;
        }
        case DevSmokeAction::None: break;
    }
    if (transport.isConnected()) transport.poll();
    heartbeat(now);
    delay(10);
}
