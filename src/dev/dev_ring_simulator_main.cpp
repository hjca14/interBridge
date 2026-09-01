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

#include <string>
#include <vector>
#include "interbridge_dev_secrets.h"
#include "dev_ring_simulator_config.h"
#include "dev_ring_button.h"
#include "dev_ring_event.h"
#include "mqtt_smoke_state.h"
#include "dev_wifi_diagnostics.h"
#include "dev_command_diagnostics.h"
#include "dev_command_environment.h"
#include "../hardware/clock.h"
#include "../hardware/ntp_sync_state.h"
#include "../core/random_id.h"
#include "../core/version.h"
#include "../network/health_reporter.h"
#include "../network/mqtt_topics.h"
#include "../network/mqtt_transport.h"
#include "../protocol/event_outbox.h"
#include "../protocol/messages.h"
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
// This pass (call-session simulator): GPIO4 still only ever starts a
// simulated call (RING_DETECTED), and GPIO3 now simulates that same
// call's end (RING_ENDED), correlated by a shared call_id - a minimal
// two-state (Idle/Ringing) machine owned by DevRingEventCoordinator, plus
// a DEV-only kDevCallTimeoutMs safety timeout so a simulated call can
// never remain Ringing forever if GPIO3 never pulses. This is still
// exclusively a bench/DEV convenience for exercising
// firmware -> MQTT -> backend -> push -> app: neither GPIO simulates real
// intercom-line ringing/off-hook/audio, and neither the Si3050 nor the
// Linker Button module is validated by this environment - see
// docs/dev-ring-simulator.md > "Call session state machine" and
// dev_ring_simulator_config.h for the full pin rationale.
//
// Cumulative DEV integration pass: this environment now also subscribes to
// the commands topic and processes them through DevCommandEnvironment (see
// dev_command_environment.h) - the exact same
// RemoteCommandProcessor/CommandHandler/InMemoryDedupCache/DisabledHardware/
// DisabledSystemControl composition esp32-c3-dev-mqtt uses, now owned by one
// shared class instead of hand-copied per entry point, so the two DEV
// environments cannot silently diverge on it again. A valid OPEN_DOOR still
// only ever reaches ACCEPTED then REJECTED/CAPABILITY_DISABLED; nothing here
// actuates a door or performs a system action. See
// docs/dev-ring-simulator.md > "Command processing (Phase 3B.8 cumulative
// pass)" and CONTEXT.md's DEV environment evolution rule.
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
// DEV backend considers a device stale after 120s - see
// mqtt_smoke_main.cpp's identical constant/rationale.
constexpr uint32_t kDevHealthIntervalMs = 60u * 1000u;

// Must match include/interbridge_dev_secrets.example.h exactly - see
// diagnoseCredentialField()'s doc comment.
constexpr const char* kWifiSsidPlaceholder = "REPLACE_WITH_WIFI_SSID";
constexpr const char* kWifiPasswordPlaceholder = "REPLACE_WITH_WIFI_PASSWORD";

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

// Reads one of the two DEV bench inputs (GPIO4 "start"/GPIO3 "end" - see
// dev_ring_simulator_config.h). Both are wired the same way: normally LOW
// through an external ~10 kOhm resistor to GND, pulsed momentarily to
// 3V3 - the exact scheme the successful GPIO4 hardware validation used
// (see docs/dev-ring-simulator.md > Bench test history). Plain INPUT (no
// internal pull-up/pull-down - the external resistor already defines the
// resting level) + active-high. This class is intentionally pin-agnostic
// so the same code path is exercised for both buttons; an earlier version
// of this file also documented an alternative "Linker Button" module
// wiring for GPIO4 (VCC/GND/SIG, self-driven both ways) - that module is
// still not validated and is not what this class assumes today.
class Esp32DevRingButtonInput final : public IDevRingButtonInput {
public:
    explicit Esp32DevRingButtonInput(uint8_t pin) : pin_(pin) {}
    bool isPressed() override { return digitalRead(pin_) == HIGH; }

private:
    uint8_t pin_;
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
// GPIO4 (start of a simulated call, RING_DETECTED) and GPIO3 (end of the
// same simulated call, RING_ENDED) - see dev_ring_simulator_config.h for
// the full pin rationale and docs/dev-ring-simulator.md > "Call session
// state machine" for the two-state coordinator both feed into.
Esp32DevRingButtonInput startButtonInput(kDevRingButtonPin);
Esp32DevRingButtonInput endButtonInput(kDevRingEndButtonPin);
DevRingButtonController startButtonController(startButtonInput);
DevRingButtonController endButtonController(endButtonInput);
DevRingEventCoordinator ringCoordinator(startButtonController, endButtonController, randomSource, eventOutbox,
                                        INTERBRIDGE_DEV_DEVICE_ID);
MqttTopics topics(devMqttTopicsConfig(INTERBRIDGE_DEV_DEVICE_ID));
Esp32AwsIotTransport transport(connectionConfig(), credentials);
// Shared DEV command-processing composition (InMemoryDedupCache/
// DisabledHardware/Intercom/DisabledSystemControl/CommandHandler/
// RemoteCommandProcessor), the same class esp32-c3-dev-mqtt uses - see
// src/dev/dev_command_environment.h and docs/dev-ring-simulator.md >
// "Command processing (Phase 3B.8 cumulative pass)". A valid OPEN_DOOR
// still only ever reaches ACCEPTED then REJECTED/CAPABILITY_DISABLED; no
// door/system action is ever genuinely actuated by this bench firmware.
DevCommandEnvironment commandEnv(INTERBRIDGE_DEV_DEVICE_ID, clockSource, transport, topics);
DevMqttSmokeState connectivity;
// Same presence contract mqtt_smoke_main.cpp publishes (HealthReport via
// Basic Ingest, see publishHealth() below) - added after a real bench
// test reached `state mqtt -> online` (local Wi-Fi/MQTT connectivity)
// while the app still showed the device offline, since this simulator
// had never published the periodic signal the backend/app presence
// contract is believed to depend on. See docs/dev-ring-simulator.md >
// "Online status: local connectivity vs. app presence" for exactly what
// is confirmed here (the firmware-side contract, verified in this repo)
// versus what is not (the backend/app's actual consumption of it, which
// lives outside this repo and has not been inspected).
HealthReporter healthReporter(kDevHealthIntervalMs);
uint32_t heartbeatAt = 0;
DevSmokeState lastLoggedState = DevSmokeState::WaitingForWifi;
// Whether the command topic is currently subscribed - mirrors
// mqtt_smoke_main.cpp's identical `subscribed` bookkeeping exactly: it
// both gates a stale-session teardown (see loop()'s top block) and, once
// this pass adds real command processing, must be re-established after
// every reconnect, never assumed to survive one.
bool subscribed = false;

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

// Sanitized, closed-set label for a scan result's auth mode - see
// mqtt_smoke_main.cpp's identical helper for the rationale. Auth mode is
// a public AP property (broadcast in every beacon frame), never secret.
const char* sanitizedAuthModeName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa_psk";
        case WIFI_AUTH_WPA2_PSK: return "wpa2_psk";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa_wpa2_psk";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
        case WIFI_AUTH_WPA3_PSK: return "wpa3_psk";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2_wpa3_psk";
        case WIFI_AUTH_WAPI_PSK: return "wapi_psk";
        default: return "unknown";
    }
}

// See mqtt_smoke_main.cpp's identical state for the full rationale.
WifiScanSummary lastWifiScanSummary;
uint32_t lastWifiScanAtMs = 0;
bool wifiScanEverRun = false;

std::string credentialConfigLine() {
    auto ssidDiag = diagnoseCredentialField(INTERBRIDGE_DEV_WIFI_SSID, kWifiSsidPlaceholder);
    auto passwordDiag = diagnoseCredentialField(INTERBRIDGE_DEV_WIFI_PASSWORD, kWifiPasswordPlaceholder);
    return formatCredentialConfigLine(summarizeCredentialConfig(ssidDiag, passwordDiag));
}

// See mqtt_smoke_main.cpp's identical function for the full rationale -
// called at boot, before every WiFi.begin(), and from the heartbeat while
// Wi-Fi is down.
void logWifiConfigAndScanSummary(uint32_t now) {
    Serial.printf("[DEV RING] %s\n", credentialConfigLine().c_str());
    if (!wifiScanEverRun) {
        Serial.println("[DEV RING] wifi scan not_yet_run=true");
        return;
    }
    const uint32_t scanAgeMs = DevMqttSmokeState::millisUntil(now, lastWifiScanAtMs);
    Serial.printf("[DEV RING] wifi scan %s\n", formatWifiScanLine(lastWifiScanSummary, scanAgeMs).c_str());
}

// See mqtt_smoke_main.cpp's identical function. Runs exactly once per
// boot - the scan is a bench diagnostic, not part of the product, and a
// real hardware test showed WiFi.scanNetworks() can itself return an
// error (-2 observed) after several association attempts, which is
// exactly the kind of diagnostic-vs-association interference a single
// boot-time scan avoids entirely. Only ever called from the ConnectWifi
// handler in loop(), strictly before that same call's WiFi.begin(), so
// it can never overlap with an association attempt; a manual reboot is
// what allows a fresh scan, not an automatic retry/interval policy.
void performWifiScan(uint32_t now) {
    WiFi.mode(WIFI_STA);
    const int16_t count = WiFi.scanNetworks();
    if (count < 0) {
        // Distinct from a scan that succeeded but found zero networks -
        // see makeFailedWifiScanSummary()/formatWifiScanLine().
        lastWifiScanSummary = makeFailedWifiScanSummary(static_cast<int32_t>(count));
    } else {
        std::vector<WifiScanNetwork> networks;
        networks.reserve(static_cast<size_t>(count));
        for (int16_t i = 0; i < count; ++i) {
            WifiScanNetwork network;
            network.ssid = std::string(WiFi.SSID(i).c_str());
            network.rssi = WiFi.RSSI(i);
            network.channel = WiFi.channel(i);
            network.authType = sanitizedAuthModeName(WiFi.encryptionType(i));
            networks.push_back(std::move(network));
        }
        lastWifiScanSummary = summarizeWifiScan(networks, INTERBRIDGE_DEV_WIFI_SSID);
    }
    WiFi.scanDelete();
    lastWifiScanAtMs = now;
    wifiScanEverRun = true;
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
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    Serial.printf("[DEV RING] local_status=%s wifi=%s time=%s mqtt=%s outbox_size=%u uptime_s=%lu\n",
                  stateName(connectivity.state()), wifiUp ? "up" : "down",
                  clockSource.hasValidTime() ? "valid" : "pending", transport.isConnected() ? "up" : "down",
                  static_cast<unsigned>(eventOutbox.size()), static_cast<unsigned long>(now / 1000));
    // Repeats the sanitized credential/scan summary while Wi-Fi is down, so
    // it stays visible even if the serial monitor is attached well after
    // boot and missed the setup()/ConnectWifi prints.
    if (!wifiUp) logWifiConfigAndScanSummary(now);
}

// Publishes the same periodic presence signal mqtt_smoke_main.cpp does
// (HealthReport via AWS IoT Basic Ingest, MqttTopics::healthIngest(),
// QoS AtMostOnce) - see the composition-root comment above for what this
// does and does not confirm about app-visible online status. This is
// entirely independent of the RING_DETECTED event outbox: a failed or
// skipped health publish here never touches eventOutbox, and a failed
// health publish is never retried through the outbox (matching
// production/DEV-MQTT's own fire-and-forget QoS 0 health semantics).
void publishHealth(uint32_t now) {
    if (WiFi.status() != WL_CONNECTED || !clockSource.hasValidTime() || !transport.isConnected() ||
        !healthReporter.isDue(now)) {
        return;
    }
    HealthReport health;
    health.deviceId = INTERBRIDGE_DEV_DEVICE_ID;
    health.firmwareVersion = FIRMWARE_VERSION;
    health.intercomState = toString(ProtocolIntercomState::Idle);
    health.uptimeMs = now;
    health.wifiRssi = WiFi.RSSI();
    health.freeHeapBytes = ESP.getFreeHeap();
    safeStatus("health publish", transport.publish(topics.healthIngest(), health.toJson(), MqttQos::AtMostOnce));
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + kSerialWaitMs;
    while (!Serial && !DevMqttSmokeState::deadlineReached(millis(), serialDeadline)) delay(10);
    Serial.println("[DEV RING] bench-only physical ring simulator; not production firmware");
    Serial.println("[DEV RING] local configuration loaded into transient DEV memory (values not logged)");
    Serial.printf("[DEV RING] previous_reset=%s\n", resetReasonName(esp_reset_reason()));

    // Registered before the first ConnectWifi action ever fires (that
    // only happens once loop() runs) so no early disconnect/connect event
    // can be missed - same ordering as mqtt_smoke_main.cpp.
    WiFi.onEvent(onWifiEvent);
    logWifiConfigAndScanSummary(millis());

    // No internal pull-up/pull-down: each external ~10 kOhm resistor to
    // GND already defines the resting LOW level - see
    // Esp32DevRingButtonInput's doc comment. Enabling an internal pull
    // here would fight that external resistor rather than complement it.
    pinMode(kDevRingButtonPin, INPUT);
    Serial.printf("[DEV RING] start button initialized gpio=%u mode=INPUT active=high rig=external_pulldown_10k "
                  "role=call_start\n",
                  static_cast<unsigned>(kDevRingButtonPin));
    pinMode(kDevRingEndButtonPin, INPUT);
    Serial.printf("[DEV RING] end button initialized gpio=%u mode=INPUT active=high rig=external_pulldown_10k "
                  "role=call_end\n",
                  static_cast<unsigned>(kDevRingEndButtonPin));

    credentials.saveCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    credentials.savePrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    sntp_set_time_sync_notification_cb([](struct timeval*) { clockSource.syncCompleted(millis()); });
    commandEnv.setDiagnosticCallback([](const CommandDiagnostic &event) {
        logCommandDiagnostic("[DEV RING]", event);
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
        // instead of only discovering this implicitly on the next connect() -
        // same defensive pattern as mqtt_smoke_main.cpp.
        Serial.println("[DEV RING] transport session invalidated; tearing down before reconnect");
        transport.disconnect();
        subscribed = false;
    }
    if (!transport.isConnected()) subscribed = false;

    drainWifiEventSignals(now);
    const DevSmokeAction action =
        connectivity.update(now, wifiConnected, clockSource.hasValidTime(), transport.isConnected());
    if (connectivity.state() != lastLoggedState) {
        Serial.printf("[DEV RING] state %s -> %s\n", stateName(lastLoggedState), stateName(connectivity.state()));
        lastLoggedState = connectivity.state();
    }
    switch (action) {
        case DevSmokeAction::ConnectWifi: {
            // Exactly one scan per boot - see performWifiScan()'s doc
            // comment - strictly sequential with (never concurrent with)
            // the WiFi.begin() below.
            if (!wifiScanEverRun) performWifiScan(now);
            logWifiConfigAndScanSummary(now);

            // The state machine authorizes exactly one begin call per retry;
            // leave interface recovery policy to the Wi-Fi driver for now.
            // A fresh timestamp, not the possibly-stale `now` from the top
            // of this loop() iteration - the rescan above may have taken
            // real, blocking time, and wifiAssociationStarted() must
            // re-arm the association timeout from the real begin moment.
            const uint32_t associationStartMs = millis();
            connectivity.wifiAssociationStarted(associationStartMs);
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.printf("[DEV RING] Wi-Fi connect requested; next_attempt_ms=%lu delay_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.retryAtMs()),
                          static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
            break;
        }
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            Serial.printf("[DEV RING] DNS: %s\n", resolvedOk ? "ready" : "pending");
            if (!resolvedOk) {
                Serial.printf("[DEV RING] next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
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
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
                break;
            }

            const bool connected =
                clockSource.hasValidTime() && transport.connect(MqttTopics::clientId(INTERBRIDGE_DEV_DEVICE_ID));
            if (connected) {
                subscribed = commandEnv.subscribe();
                safeStatus("command QoS1 subscription", subscribed);
                if (!subscribed) {
                    transport.disconnect();
                } else {
                    healthReporter.forceNextPublish();
                    Serial.printf("[DEV RING] reconnected; pending_responses=%u\n",
                                  static_cast<unsigned>(commandEnv.pendingResponseCount()));
                }
            }
            connectivity.mqttResult(now, connected && subscribed);
            safeStatus("MQTT connect", connected && subscribed);
            if (!connected || !subscribed) {
                Serial.printf("[DEV RING] connectivity_failures=%lu next MQTT attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.consecutiveConnectivityFailures()),
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
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
    const DevCallSessionOutcome outcome =
        ringCoordinator.update(now, clockSource.hasValidTime(), clockSource.unixTimeSeconds());
    bool eventEnqueuedThisLoop = false;
    switch (outcome.kind) {
        case DevCallSessionEventKind::RingDetected:
            eventEnqueuedThisLoop = true;
            Serial.printf("[DEV RING] valid start detected on GPIO%u; RING_DETECTED enqueued call_id=%s\n",
                          static_cast<unsigned>(kDevRingButtonPin), outcome.callId.c_str());
            break;
        case DevCallSessionEventKind::RingEndedByButton:
            eventEnqueuedThisLoop = true;
            Serial.printf("[DEV RING] valid end detected on GPIO%u; RING_ENDED enqueued call_id=%s\n",
                          static_cast<unsigned>(kDevRingEndButtonPin), outcome.callId.c_str());
            break;
        case DevCallSessionEventKind::RingEndedByTimeout:
            eventEnqueuedThisLoop = true;
            // The timeout reason is deliberately local-log-only, not a
            // wire payload field: docs/communication-protocol.md does not
            // yet define one, and this DEV simulator must not invent a
            // field the backend was never coordinated on - see
            // docs/dev-ring-simulator.md > "Call session state machine" >
            // Safety timeout.
            Serial.printf("[DEV RING] call timed out after %lums with no GPIO%u pulse; RING_ENDED enqueued "
                          "call_id=%s reason=timeout(local-only)\n",
                          static_cast<unsigned long>(kDevCallTimeoutMs), static_cast<unsigned>(kDevRingEndButtonPin),
                          outcome.callId.c_str());
            break;
        case DevCallSessionEventKind::StartIgnoredAlreadyRinging:
            Serial.printf("[DEV RING] GPIO%u press ignored; call_id=%s already active\n",
                          static_cast<unsigned>(kDevRingButtonPin), outcome.callId.c_str());
            break;
        case DevCallSessionEventKind::EndIgnoredNoActiveCall:
            Serial.printf("[DEV RING] GPIO%u press ignored; no active call\n",
                          static_cast<unsigned>(kDevRingEndButtonPin));
            break;
        case DevCallSessionEventKind::None:
            break;
    }

    if (transport.isConnected()) {
        transport.poll();
        commandEnv.processPending();
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
        if (eventEnqueuedThisLoop) {
            Serial.println("[DEV RING] offline; event queued, awaiting reconnection");
        }
    }

    publishHealth(now);
    heartbeat(now);
    delay(10);
}
