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
#include <esp_system.h>

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
//
// The Wi-Fi event handler, per-action diagnostic log lines, and boot
// diagnostics below deliberately mirror mqtt_smoke_main.cpp's own
// (already real-hardware-validated - see docs/mqtt-dev-smoke-test.md)
// pattern line for line, rather than inventing a different logging
// mechanism, after a first bench boot of this environment produced only
// "wifi=down" heartbeats for 120s with no way to tell whether
// WiFi.begin() was ever reached, whether an association attempt actually
// failed (and why), or whether a silent reset loop occurred - none of
// that was observable before this pass. A full side-by-side comparison
// of the two files' connectivity bring-up did not find a functional
// difference: both drive the identical DevMqttSmokeState state machine
// with WiFi.mode(WIFI_STA) + WiFi.begin() issued only on its ConnectWifi
// action, the same retry/backoff policy, WiFi.disconnect(false, false)
// only on RecoverWifi (radio stays on, credentials are kept), no periodic
// restart, and identical build flags/board. This pass therefore closes an
// observability gap, not a proven logic bug - see docs/dev-ring-
// simulator.md > Honest status for what the next real hardware boot
// still needs to confirm.
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

// Names only the specific SDK-defined reason codes observed on real
// hardware, referencing esp_wifi_types.h's own named constants directly -
// not a hand-copied duplicate of the whole wifi_err_reason_t enum (which
// would drift against future core versions). Anything else stays
// numeric-only in the log line already printed alongside this. Mirrors
// mqtt_smoke_main.cpp's identical helper verbatim.
const char* wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
        case WIFI_REASON_TIMEOUT: return "timeout";
        default: return nullptr;
    }
}

// Set only from the Wi-Fi event callback below (which the ESP32 Arduino
// core runs on its own Wi-Fi/event task, not the main loop task) and
// consumed only from loop() via drainWifiEventSignals() - see the
// identical pattern/rationale in mqtt_smoke_main.cpp (this state machine
// is shared via DevMqttSmokeState; only this small amount of Arduino-main
// glue is necessarily duplicated per entry point, same as stateName()/
// safeStatus()/NtpClock already are). No DevMqttSmokeState method is ever
// called directly from this callback.
volatile bool wifiAssociatedEventPending = false;
volatile bool wifiDisconnectedEventPending = false;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        wifiDisconnectedEventPending = true;
        const char* name = wifiDisconnectReasonName(reason);
        if (name) {
            Serial.printf("[DEV RING] wifi event=disconnected reason=%u (%s)\n",
                          static_cast<unsigned>(reason), name);
        } else {
            Serial.printf("[DEV RING] wifi event=disconnected reason=%u\n",
                          static_cast<unsigned>(reason));
        }
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        wifiAssociatedEventPending = true;
        Serial.println("[DEV RING] wifi event=connected");
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        wifiAssociatedEventPending = true;
        Serial.println("[DEV RING] wifi event=got_ip");
    }
}

// Drains whatever the event callback recorded above and forwards it as an
// explicit, synchronous DevMqttSmokeState::wifiAssociationResult() call -
// called once per loop() iteration, before connectivity.update().
void drainWifiEventSignals(uint32_t now) {
    if (wifiAssociatedEventPending) {
        wifiAssociatedEventPending = false;
        connectivity.wifiAssociationResult(now, true);
    }
    if (wifiDisconnectedEventPending) {
        wifiDisconnectedEventPending = false;
        connectivity.wifiAssociationResult(now, false);
    }
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
    Serial.printf("[DEV RING] previous_reset=%s wifi_config=present\n", resetReasonName(esp_reset_reason()));

    // Registered before the first ConnectWifi action ever fires (that
    // only happens once loop() runs) so no early disconnect/connect event
    // can be missed - same ordering as mqtt_smoke_main.cpp.
    WiFi.onEvent(onWifiEvent);

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

    drainWifiEventSignals(now);
    const DevSmokeAction action =
        connectivity.update(now, wifiConnected, clockSource.hasValidTime(), transport.isConnected());
    if (connectivity.state() != lastLoggedState) {
        Serial.printf("[DEV RING] state %s -> %s\n", stateName(lastLoggedState), stateName(connectivity.state()));
        lastLoggedState = connectivity.state();
    }
    switch (action) {
        case DevSmokeAction::ConnectWifi:
            // The state machine authorizes exactly one begin call per retry;
            // leave interface recovery policy to the Wi-Fi driver for now.
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.printf("[DEV RING] Wi-Fi connect requested; next_attempt_ms=%lu delay_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.retryAtMs()),
                          static_cast<unsigned long>(connectivity.retryAtMs() - now));
            break;
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            Serial.printf("[DEV RING] DNS: %s\n", resolvedOk ? "ready" : "pending");
            if (!resolvedOk) {
                Serial.printf("[DEV RING] next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(connectivity.retryAtMs() - now));
            }
            break;
        }
        case DevSmokeAction::ConfigureTime:
            clockSource.syncStarted();
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            Serial.println("[DEV RING] time sync requested");
            break;
        case DevSmokeAction::ConnectMqtt: {
            // DNS preflight, same rationale as mqtt_smoke_main.cpp: a
            // Wi-Fi association surviving does not prove the resolver is
            // actually working - resolve explicitly, once, before ever
            // attempting the heavier TLS/socket connect.
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            IPAddress preflightResolved;
            const bool dnsOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, preflightResolved) == 1;
            Serial.printf("[DEV RING] network preflight dns=%s\n", dnsOk ? "ok" : "failed");
            if (!dnsOk) {
                connectivity.networkPreflightFailed(now);
                Serial.printf("[DEV RING] connectivity_failures=%lu next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.consecutiveConnectivityFailures()),
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(connectivity.retryAtMs() - now));
                break;
            }

            const bool connected =
                clockSource.hasValidTime() && transport.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID));
            connectivity.mqttResult(now, connected);
            safeStatus("MQTT connect", connected);
            if (!connected) {
                Serial.printf("[DEV RING] connectivity_failures=%lu next MQTT attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.consecutiveConnectivityFailures()),
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(connectivity.retryAtMs() - now));
            }
            break;
        }
        case DevSmokeAction::RecoverWifi:
            // Authorized only after several consecutive DNS/TLS connectivity
            // failures with Wi-Fi already associated - a conservative,
            // cooldown-limited escalation, never a periodic reboot and
            // never ESP.restart().
            Serial.println("[DEV RING] wifi recovery requested");
            if (transport.isConnected()) transport.disconnect();
            // wifioff=false keeps the radio on; eraseap=false keeps the
            // stored Wi-Fi credentials - only the current association is
            // dropped. The ordinary ConnectWifi/backoff flow (unchanged)
            // re-associates from here.
            WiFi.disconnect(false, false);
            Serial.printf("[DEV RING] wifi recovery cooldown until_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.wifiRecoveryCooldownUntilMs()));
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
