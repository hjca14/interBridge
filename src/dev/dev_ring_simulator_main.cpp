#ifndef INTERBRIDGE_DEV_RING_SIMULATOR
#error "dev_ring_simulator_main.cpp is only for INTERBRIDGE_DEV_RING_SIMULATOR"
#endif
#if !__has_include("interbridge_dev_secrets.h")
#error "DEV ring simulator build requires ignored include/interbridge_dev_secrets.h; copy the example first"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>

#include "interbridge_dev_secrets.h"
#include "dev_ring_simulator_config.h"
#include "dev_ring_button.h"
#include "dev_ring_event.h"
#include "mqtt_smoke_state.h"
#include "../hardware/clock.h"
#include "../hardware/ntp_sync_state.h"
#include "../core/random_id.h"
#include "../core/version.h"
#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "../protocol/event_outbox.h"
#include "../storage/credential_store.h"
#include "../storage/memory_store.h"

// Phase 3B.8 bench-only DEV physical ring simulator. See
// docs/dev-ring-simulator.md for scope, wiring, and the manual flash/test
// procedure. This is NOT production firmware and NEVER touches the real
// Si3050/RingDetector/PCM clock, provisioning, BLE, or production Wi-Fi/
// AWS composition (main.cpp). It reuses the DEV MQTT smoke environment's
// connectivity bring-up (DevMqttSmokeState, defined in mqtt_smoke_state.*
// and already compiled into this environment) instead of duplicating it,
// and publishes through the exact same AWS IoT Basic Ingest event flow as
// production: MqttTopics::eventsIngest(), Esp32AwsIotTransport, and the
// existing IEventOutbox contract - see DevRingEventCoordinator/
// publishPendingEvents (src/dev/dev_ring_event.*).
namespace {
using namespace interbridge;
constexpr uint16_t kMqttPort = 8883;
constexpr uint16_t kMqttKeepAliveSeconds = 300;
constexpr uint16_t kMqttTimeoutMs = 1000;
constexpr uint32_t kSerialWaitMs = 1500;
constexpr uint32_t kHeartbeatMs = 15000;

// Mirrors mqtt_smoke_main.cpp's NtpClock exactly: hardware/clock.h's
// Esp32Clock::hasValidTime() is a deliberate stub that always reports no
// valid time (see CONTEXT.md), so this DEV-only harness needs its own
// real IClock backed by the shared NtpSyncState gate.
class NtpClock final : public IClock {
public:
    uint32_t monotonicMs() const override { return millis(); }
    void syncStarted() { syncState_.synchronizationStarted(); }
    void syncCompleted(uint32_t nowMs) { syncState_.synchronizationCompleted(nowMs); }
    bool hasValidTime() const override { return syncState_.isTrustworthy(millis(), syncInProgress()); }
    bool syncInProgress() const { return sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS; }
    int64_t unixTimeSeconds() const override { return static_cast<int64_t>(time(nullptr)); }
private:
    NtpSyncState syncState_;
};

// Reads the physical button: INPUT_PULLUP + active-low, so a raw LOW
// reading means "pressed".
class Esp32DevRingButtonInput final : public IDevRingButtonInput {
public:
    bool isPressed() override { return digitalRead(kDevRingButtonPin) == LOW; }
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
// DEV-only transient composition, same rationale as mqtt_smoke_main.cpp:
// credentials and the event outbox do not need to survive reboot on this
// bench-only simulator.
MemoryStore devStore;
DeviceCredentialStore credentials(devStore);
Esp32RandomSource randomSource;
MemoryEventOutbox eventOutbox;
Esp32DevRingButtonInput buttonInput;
DevRingButtonController buttonController(buttonInput);
DevRingEventCoordinator ringCoordinator(buttonController, randomSource, eventOutbox, INTERBRIDGE_DEV_DEVICE_ID);
MqttTopics topics(devMqttTopicsConfig(INTERBRIDGE_DEV_DEVICE_ID));
Esp32AwsIotTransport transport(connectionConfig(), credentials);
DevMqttSmokeState connectivity;
uint32_t heartbeatAt = 0;
DevSmokeState lastLoggedState = DevSmokeState::WaitingForWifi;
// Tracks whether the transport was connected as of the previous loop
// iteration, so a session invalidation (see
// Esp32AwsIotTransport::isConnected()) is torn down exactly once instead
// of calling transport.disconnect() on every tick while already
// disconnected - mirrors mqtt_smoke_main.cpp's `subscribed` bookkeeping,
// which this environment does not need since it never subscribes.
bool wasConnected = false;

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

void safeStatus(const char* operation, bool ok) {
    Serial.printf("[DEV RING] %s: %s\n", operation, ok ? "ok" : "failed");
}

void heartbeat(uint32_t now) {
    if (!DevMqttSmokeState::deadlineReached(now, heartbeatAt)) return;
    heartbeatAt = now + kHeartbeatMs;
    Serial.printf("[DEV RING] local_status=%s wifi=%s time=%s mqtt=%s outbox_size=%u uptime_s=%lu\n",
                  stateName(connectivity.state()), WiFi.status() == WL_CONNECTED ? "up" : "down",
                  clockSource.hasValidTime() ? "valid" : "pending", transport.isConnected() ? "up" : "down",
                  static_cast<unsigned>(eventOutbox.size()), static_cast<unsigned long>(now / 1000));
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + kSerialWaitMs;
    while (!Serial && !DevMqttSmokeState::deadlineReached(millis(), serialDeadline)) delay(10);
    Serial.println("[DEV RING] bench-only physical ring simulator; not production firmware");
    Serial.println("[DEV RING] local configuration loaded into transient DEV memory (values not logged)");

    pinMode(kDevRingButtonPin, INPUT_PULLUP);
    Serial.printf("[DEV RING] button initialized gpio=%u (INPUT_PULLUP, active-low)\n",
                  static_cast<unsigned>(kDevRingButtonPin));

    credentials.saveCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    credentials.savePrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    sntp_set_time_sync_notification_cb([](struct timeval*) { clockSource.syncCompleted(millis()); });
}

void loop() {
    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if ((!wifiConnected || !clockSource.hasValidTime()) && transport.isConnected()) {
        transport.disconnect();
    } else if (!transport.isConnected() && wasConnected) {
        // A publish/poll failure already invalidated the session (see
        // Esp32AwsIotTransport::isConnected()) - tear down explicitly so
        // the next attempt never reuses a stale socket, same defensive
        // pattern as mqtt_smoke_main.cpp. Gated on wasConnected so this
        // only fires once per invalidation, not every tick while already
        // disconnected.
        transport.disconnect();
    }
    wasConnected = transport.isConnected();

    const DevSmokeAction action =
        connectivity.update(now, wifiConnected, clockSource.hasValidTime(), transport.isConnected());
    if (connectivity.state() != lastLoggedState) {
        Serial.printf("[DEV RING] state %s -> %s\n", stateName(lastLoggedState), stateName(connectivity.state()));
        lastLoggedState = connectivity.state();
    }
    switch (action) {
        case DevSmokeAction::ConnectWifi:
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            break;
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            break;
        }
        case DevSmokeAction::ConfigureTime:
            clockSource.syncStarted();
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            break;
        case DevSmokeAction::ConnectMqtt: {
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            IPAddress preflightResolved;
            const bool dnsOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, preflightResolved) == 1;
            if (!dnsOk) {
                connectivity.networkPreflightFailed(now);
                break;
            }
            const bool connected =
                clockSource.hasValidTime() && transport.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID));
            connectivity.mqttResult(now, connected);
            safeStatus("MQTT connect", connected);
            break;
        }
        case DevSmokeAction::RecoverWifi:
            if (transport.isConnected()) transport.disconnect();
            WiFi.disconnect(false, false);
            break;
        case DevSmokeAction::None: break;
    }

    // Button handling and event enqueueing happen every loop iteration,
    // independent of connectivity state - a press while offline still
    // enqueues (see DevRingEventCoordinator::update()) and is replayed
    // once the outbox drain below can reach the broker again.
    std::string eventId = ringCoordinator.update(now, clockSource.hasValidTime(), clockSource.unixTimeSeconds());
    if (!eventId.empty()) {
        Serial.println("[DEV RING] valid press detected; RING_DETECTED enqueued");
    }

    if (transport.isConnected()) {
        transport.poll();
        if (eventOutbox.size() > 0) {
            size_t publishedCount = publishPendingEvents(eventOutbox, transport, topics.eventsIngest());
            if (publishedCount > 0) {
                Serial.printf("[DEV RING] publish confirmed count=%u remaining=%u\n",
                              static_cast<unsigned>(publishedCount), static_cast<unsigned>(eventOutbox.size()));
            }
        }
    } else if (eventOutbox.size() > 0) {
        // Logged at heartbeat cadence too, but an explicit line right
        // after a fresh enqueue makes the "waiting for reconnection"
        // state obvious without waiting up to 15s for the heartbeat.
        if (!eventId.empty()) {
            Serial.println("[DEV RING] offline; event queued, awaiting reconnection");
        }
    }

    heartbeat(now);
    delay(10);
}
