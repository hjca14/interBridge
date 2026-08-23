#ifndef INTERBRIDGE_DEV_MQTT_SMOKE
#error "mqtt_smoke_main.cpp is only for INTERBRIDGE_DEV_MQTT_SMOKE"
#endif
#if !__has_include("interbridge_dev_secrets.h")
#error "DEV MQTT smoke build requires ignored include/interbridge_dev_secrets.h; copy the example first"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "interbridge_dev_secrets.h"
#include "mqtt_smoke_state.h"
#include "../hardware/clock.h"
#include "../hardware/ntp_sync_state.h"
#include "../hardware/gpio.h"
#include "../hardware/system_control.h"
#include "../intercom/intercom.h"
#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "../network/health_reporter.h"
#include "../protocol/messages.h"
#include "../core/version.h"
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
// DEV backend considers a device stale after 120s. Publishing halfway through
// that window leaves one missed-report margin without using the 15s log cadence.
constexpr uint32_t kDevHealthIntervalMs = 60u * 1000u;

class NtpClock final : public IClock {
public:
    uint32_t monotonicMs() const override { return millis(); }
    void syncStarted() { syncState_.synchronizationStarted(); }
    void syncCompleted(uint32_t nowMs) { syncState_.synchronizationCompleted(nowMs); }
    bool hasValidTime() const override {
        return syncState_.isTrustworthy(millis(), syncInProgress());
    }
    // Exposed so the connectivity state machine can avoid reissuing
    // configTime() while a previous attempt may still be in flight - see
    // DevMqttSmokeState::update()'s timeSyncInProgress parameter.
    bool syncInProgress() const { return sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS; }
    int64_t unixTimeSeconds() const override { return static_cast<int64_t>(time(nullptr)); }
private:
    NtpSyncState syncState_;
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
HealthReporter healthReporter(kDevHealthIntervalMs);
DevSmokeState lastLoggedState = DevSmokeState::WaitingForWifi;

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "other";
    }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("[DEV MQTT] wifi event=disconnected reason=%u\n",
                      static_cast<unsigned>(info.wifi_sta_disconnected.reason));
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        Serial.println("[DEV MQTT] wifi event=connected");
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.println("[DEV MQTT] wifi event=got_ip");
    }
}

void onTimeSynchronized(struct timeval*) { clockSource.syncCompleted(millis()); }

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
    Serial.printf("[DEV MQTT] local_status=%s wifi=%s time=%s mqtt=%s uptime_s=%lu heap_free=%u heap_min=%u stack_words=%u\n",
                  stateName(connectivity.state()), WiFi.status() == WL_CONNECTED ? "up" : "down",
                  clockSource.hasValidTime() ? "valid" : "pending",
                  transport.isConnected() ? "up" : "down",
                  static_cast<unsigned long>(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void publishHealth(uint32_t now) {
    if (!subscribed || WiFi.status() != WL_CONNECTED || !clockSource.hasValidTime() ||
        !transport.isConnected() || !healthReporter.isDue(now)) return;
    HealthReport health;
    health.deviceId = INTERBRIDGE_DEV_DEVICE_ID;
    health.firmwareVersion = FIRMWARE_VERSION;
    health.intercomState = toString(ProtocolIntercomState::Idle);
    health.uptimeMs = now;
    health.wifiRssi = WiFi.RSSI();
    health.freeHeapBytes = ESP.getFreeHeap();
    safeStatus("health publish", transport.publish(topics.healthIngest(), health.toJson(),
                                                    MqttQos::AtMostOnce));
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + kSerialWaitMs;
    while (!Serial && !DevMqttSmokeState::deadlineReached(millis(), serialDeadline)) delay(10);
    Serial.println("[DEV MQTT] production-path harness; physical actions disabled");
    Serial.println("[DEV MQTT] local configuration loaded into transient DEV memory (values not logged)");
    Serial.printf("[DEV MQTT] previous_reset=%s wifi_config=present\n",
                  resetReasonName(esp_reset_reason()));
    WiFi.onEvent(onWifiEvent);
    credentials.saveCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    credentials.savePrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    sntp_set_time_sync_notification_cb(onTimeSynchronized);
    processor.setDiagnosticCallback([](const CommandDiagnostic &event) {
        switch (event.stage) {
            case CommandDiagnosticStage::Received:
                // No seq yet here - it is assigned once the command reaches
                // the front of the queue and starts processing (see the
                // stages below), not at raw MQTT delivery time.
                Serial.println("[DEV MQTT] command received"); break;
            case CommandDiagnosticStage::ValidationPassed:
                Serial.printf("[DEV MQTT] time validation ok seq=%lu age_s=%lld remaining_s=%lld\n",
                              static_cast<unsigned long>(event.commandSeq),
                              static_cast<long long>(event.ageSeconds),
                              static_cast<long long>(event.remainingSeconds)); break;
            case CommandDiagnosticStage::Rejected:
                Serial.printf("[DEV MQTT] command rejected seq=%lu code=%s\n",
                              static_cast<unsigned long>(event.commandSeq), event.safeCode); break;
            case CommandDiagnosticStage::AcceptedPublished:
                Serial.printf("[DEV MQTT] ACCEPTED published seq=%lu\n",
                              static_cast<unsigned long>(event.commandSeq)); break;
            case CommandDiagnosticStage::AcceptedPending:
                Serial.printf("[DEV MQTT] ACCEPTED pending (publish failed; queued) seq=%lu\n",
                              static_cast<unsigned long>(event.commandSeq)); break;
            case CommandDiagnosticStage::TerminalPublished:
                // event.safeCode here is the device's own terminal status/error code
                // (e.g. CAPABILITY_DISABLED), never a transport/publish artifact.
                Serial.printf("[DEV MQTT] terminal published seq=%lu code=%s\n",
                              static_cast<unsigned long>(event.commandSeq), event.safeCode); break;
            case CommandDiagnosticStage::TerminalPending:
                Serial.printf("[DEV MQTT] terminal pending (publish failed; queued) seq=%lu code=%s\n",
                              static_cast<unsigned long>(event.commandSeq), event.safeCode); break;
        }
    });
}

void loop() {
    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if ((!wifiConnected || !clockSource.hasValidTime()) && transport.isConnected()) {
        transport.disconnect();
        subscribed = false;
    } else if (!transport.isConnected() && subscribed) {
        // Wi-Fi/time are still fine, but a publish/subscribe/poll failure
        // already invalidated the MQTT/TLS session (see
        // Esp32AwsIotTransport::isConnected()). Tear it down explicitly so
        // the next ConnectMqtt attempt never reuses a stale socket/session,
        // instead of only discovering this implicitly on the next connect().
        Serial.println("[DEV MQTT] transport session invalidated; tearing down before reconnect");
        transport.disconnect();
        subscribed = false;
    }
    if (!transport.isConnected()) subscribed = false;

    const DevSmokeAction action = connectivity.update(
        now, wifiConnected, clockSource.hasValidTime(), transport.isConnected(),
        clockSource.syncInProgress());
    if (connectivity.state() != lastLoggedState) {
        Serial.printf("[DEV MQTT] state %s -> %s\n", stateName(lastLoggedState), stateName(connectivity.state()));
        lastLoggedState = connectivity.state();
    }
    switch (action) {
        case DevSmokeAction::ConnectWifi:
            // The state machine authorizes exactly one begin call per retry;
            // leave interface recovery policy to the Wi-Fi driver for now.
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.printf("[DEV MQTT] Wi-Fi connect requested; next_attempt_ms=%lu delay_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.retryAtMs()),
                          static_cast<unsigned long>(connectivity.retryAtMs() - now));
            break;
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            Serial.printf("[DEV MQTT] DNS: %s\n", resolvedOk ? "ready" : "pending");
            if (!resolvedOk) {
                Serial.printf("[DEV MQTT] next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(connectivity.retryAtMs() - now));
            }
            break;
        }
        case DevSmokeAction::ConfigureTime:
            clockSource.syncStarted();
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            Serial.println("[DEV MQTT] time sync requested");
            break;
        case DevSmokeAction::ConnectMqtt: {
            const bool connected = clockSource.hasValidTime() &&
                transport.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID));
            if (connected) {
                subscribed = processor.subscribe();
                safeStatus("command QoS1 subscription", subscribed);
                if (!subscribed) {
                    transport.disconnect();
                } else {
                    healthReporter.forceNextPublish();
                    Serial.printf("[DEV MQTT] reconnected; pending_responses=%u\n",
                                  static_cast<unsigned>(processor.pendingResponseCount()));
                }
            }
            connectivity.mqttResult(now, connected && subscribed);
            safeStatus("MQTT connect", connected && subscribed);
            if (!connected || !subscribed) {
                Serial.printf("[DEV MQTT] next MQTT attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(connectivity.retryAtMs() - now));
            }
            break;
        }
        case DevSmokeAction::None: break;
    }
    if (transport.isConnected()) {
        transport.poll();
        processor.processPending();
    }
    publishHealth(now);
    heartbeat(now);
    delay(10);
}
