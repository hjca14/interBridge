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
#include <string>
#include <vector>
#include "interbridge_dev_secrets.h"
#include "mqtt_smoke_state.h"
#include "dev_wifi_diagnostics.h"
#include "dev_disabled_hardware.h"
#include "dev_command_diagnostics.h"
#include "../hardware/clock.h"
#include "../hardware/ntp_sync_state.h"
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

// Must match include/interbridge_dev_secrets.example.h exactly - see
// diagnoseCredentialField()'s doc comment.
constexpr const char* kWifiSsidPlaceholder = "REPLACE_WITH_WIFI_SSID";
constexpr const char* kWifiPasswordPlaceholder = "REPLACE_WITH_WIFI_PASSWORD";

class NtpClock final : public IClock {
public:
    uint32_t monotonicMs() const override { return millis(); }
    void syncStarted() { syncState_.synchronizationStarted(); }
    void syncCompleted(uint32_t nowMs) { syncState_.synchronizationCompleted(nowMs); }
    bool hasValidTime() const override {
        return syncState_.isTrustworthy(millis(), syncInProgress());
    }
    // Used only as part of hasValidTime()'s own settling gate (do not trust
    // time while a new resync might already be underway). NOT used to gate
    // ConfigureTime retries: DevMqttSmokeState tracks its own bounded
    // in-flight timer for that instead, since this status can stay
    // reset/idle for a while after configTime() is called and so cannot
    // serve as a reliable "is a previous attempt still running" signal.
    bool syncInProgress() const { return sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS; }
    int64_t unixTimeSeconds() const override { return static_cast<int64_t>(time(nullptr)); }
private:
    NtpSyncState syncState_;
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

// Names only the specific SDK-defined reason codes observed on real
// hardware, referencing esp_wifi_types.h's own named constants directly -
// not a hand-copied duplicate of the whole wifi_err_reason_t enum (which
// would drift against future core versions). Anything else stays
// numeric-only in the log line already printed alongside this.
const char* wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
        case WIFI_REASON_TIMEOUT: return "timeout";
        default: return nullptr;
    }
}

// Sanitized, closed-set label for a scan result's auth mode - referencing
// esp_wifi_types.h's own named wifi_auth_mode_t constants directly (same
// no-hand-copied-drift rationale as wifiDisconnectReasonName() above).
// Auth mode is a public AP property (broadcast in every beacon frame),
// never secret - "sanitized" here just means a small, stable label set
// instead of a raw driver enum value.
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

// Last completed scan's summary, its timestamp, and whether one has run
// yet at all - kept so the same sanitized line can be repeated later
// (before every WiFi.begin(), and in the heartbeat while Wi-Fi is down)
// without rescanning every time - see performWifiScan()'s doc comment
// for why exactly one scan per boot, never on a retry/failure cadence.
WifiScanSummary lastWifiScanSummary;
uint32_t lastWifiScanAtMs = 0;
bool wifiScanEverRun = false;

std::string credentialConfigLine() {
    auto ssidDiag = diagnoseCredentialField(INTERBRIDGE_DEV_WIFI_SSID, kWifiSsidPlaceholder);
    auto passwordDiag = diagnoseCredentialField(INTERBRIDGE_DEV_WIFI_PASSWORD, kWifiPasswordPlaceholder);
    return formatCredentialConfigLine(summarizeCredentialConfig(ssidDiag, passwordDiag));
}

// Logs the sanitized credential-config line and the last known scan
// summary (with a freshly computed age) - called at boot, right before
// every WiFi.begin(), and from the heartbeat while Wi-Fi is down, so this
// stays visible even if the serial monitor is attached well after boot
// and missed the earlier prints.
void logWifiConfigAndScanSummary(uint32_t now) {
    Serial.printf("[DEV MQTT] %s\n", credentialConfigLine().c_str());
    if (!wifiScanEverRun) {
        Serial.println("[DEV MQTT] wifi scan not_yet_run=true");
        return;
    }
    const uint32_t scanAgeMs = DevMqttSmokeState::millisUntil(now, lastWifiScanAtMs);
    Serial.printf("[DEV MQTT] wifi scan %s\n", formatWifiScanLine(lastWifiScanSummary, scanAgeMs).c_str());
}

// Synchronous/blocking Wi-Fi scan (WiFi.scanNetworks() with its default
// async=false) - only ever called from the ConnectWifi handler in loop(),
// strictly before that same call's WiFi.begin(), so it can never overlap
// with an association attempt: the two are sequential statements in the
// same single-threaded function call, not concurrent by construction.
// Runs exactly once per boot - the scan is a bench diagnostic, not part
// of the product, and a real hardware test showed WiFi.scanNetworks()
// can itself return an error (-2 observed) after several association
// attempts, which is exactly the kind of diagnostic-vs-association
// interference a single boot-time scan avoids entirely. A manual reboot
// is what allows a fresh scan, not an automatic retry/interval policy.
void performWifiScan(uint32_t now) {
    WiFi.mode(WIFI_STA);
    const int16_t count = WiFi.scanNetworks();
    if (count < 0) {
        // WIFI_SCAN_FAILED (or similar) - the scan call itself did not
        // complete successfully, distinct from a scan that genuinely
        // found zero networks (count == 0, handled below via an empty
        // networks list). Recorded as a real status, not silently
        // reinterpreted as "zero networks found" - see
        // logWifiConfigAndScanSummary()/formatWifiScanLine() for how this
        // stays distinguishable in every later repeated log line too.
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
// consumed only from loop() via drainWifiEventSignals(). Forwarding a real
// connected/got_ip/disconnected outcome into DevMqttSmokeState is a state
// transition, and doing that directly from a different task than the one
// that owns/reads DevMqttSmokeState everywhere else would be a genuine
// concurrency hazard - so the callback only ever records this minimal
// signal (a plain flag, never a compound operation), and the actual
// wifiAssociationResult() call happens synchronously from the ordinary,
// single-threaded main loop instead. The sanitized disconnect reason code
// is still logged immediately, synchronously, inside the callback itself
// (as before) purely for diagnostics - it never needs to reach the state
// machine.
volatile bool wifiAssociatedEventPending = false;
volatile bool wifiDisconnectedEventPending = false;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        wifiDisconnectedEventPending = true;
        const char* name = wifiDisconnectReasonName(reason);
        if (name) {
            Serial.printf("[DEV MQTT] wifi event=disconnected reason=%u (%s)\n",
                          static_cast<unsigned>(reason), name);
        } else {
            Serial.printf("[DEV MQTT] wifi event=disconnected reason=%u\n",
                          static_cast<unsigned>(reason));
        }
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        wifiAssociatedEventPending = true;
        Serial.println("[DEV MQTT] wifi event=connected");
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        wifiAssociatedEventPending = true;
        Serial.println("[DEV MQTT] wifi event=got_ip");
    }
}

// Drains whatever the event callback recorded above and forwards it as an
// explicit, synchronous DevMqttSmokeState::wifiAssociationResult() call -
// called once per loop() iteration, before connectivity.update(), so a
// resolved attempt is reflected before the state machine decides its next
// action for this tick.
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
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    Serial.printf("[DEV MQTT] local_status=%s wifi=%s time=%s mqtt=%s uptime_s=%lu heap_free=%u heap_min=%u stack_words=%u\n",
                  stateName(connectivity.state()), wifiUp ? "up" : "down",
                  clockSource.hasValidTime() ? "valid" : "pending",
                  transport.isConnected() ? "up" : "down",
                  static_cast<unsigned long>(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    // Repeats the sanitized credential/scan summary while Wi-Fi is down, so
    // it stays visible even if the serial monitor is attached well after
    // boot and missed the setup()/ConnectWifi prints.
    if (!wifiUp) logWifiConfigAndScanSummary(now);
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
    Serial.printf("[DEV MQTT] previous_reset=%s\n", resetReasonName(esp_reset_reason()));
    WiFi.onEvent(onWifiEvent);
    logWifiConfigAndScanSummary(millis());
    credentials.saveCertificate(INTERBRIDGE_DEV_CERTIFICATE_PEM);
    credentials.savePrivateKey(INTERBRIDGE_DEV_PRIVATE_KEY_PEM);
    sntp_set_time_sync_notification_cb(onTimeSynchronized);
    processor.setDiagnosticCallback([](const CommandDiagnostic &event) {
        logCommandDiagnostic("[DEV MQTT]", event);
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

    drainWifiEventSignals(now);
    const DevSmokeAction action = connectivity.update(
        now, wifiConnected, clockSource.hasValidTime(), transport.isConnected());
    if (connectivity.state() != lastLoggedState) {
        Serial.printf("[DEV MQTT] state %s -> %s\n", stateName(lastLoggedState), stateName(connectivity.state()));
        lastLoggedState = connectivity.state();
    }
    switch (action) {
        case DevSmokeAction::ConnectWifi: {
            // Exactly one scan per boot - see performWifiScan()'s doc
            // comment - strictly sequential with (and completing fully
            // before) the WiFi.begin() below, never concurrent with
            // association.
            if (!wifiScanEverRun) performWifiScan(now);
            logWifiConfigAndScanSummary(now);

            // The state machine authorizes exactly one begin call per retry;
            // leave interface recovery policy to the Wi-Fi driver for now.
            // A fresh timestamp, not the possibly-several-seconds-stale
            // `now` from the top of this loop() iteration: the rescan
            // above may have taken real, blocking time, and
            // wifiAssociationStarted() must re-arm the association
            // timeout from the moment WiFi.begin() actually fires, not
            // from before the scan - see DevMqttSmokeState's doc comment.
            const uint32_t associationStartMs = millis();
            connectivity.wifiAssociationStarted(associationStartMs);
            WiFi.mode(WIFI_STA);
            WiFi.begin(INTERBRIDGE_DEV_WIFI_SSID, INTERBRIDGE_DEV_WIFI_PASSWORD);
            Serial.printf("[DEV MQTT] Wi-Fi connect requested; next_attempt_ms=%lu delay_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.retryAtMs()),
                          static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
            break;
        }
        case DevSmokeAction::ResolveDns: {
            IPAddress resolved;
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            const bool resolvedOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, resolved) == 1;
            connectivity.dnsResult(now, resolvedOk);
            Serial.printf("[DEV MQTT] DNS: %s\n", resolvedOk ? "ready" : "pending");
            if (!resolvedOk) {
                Serial.printf("[DEV MQTT] next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
            }
            break;
        }
        case DevSmokeAction::ConfigureTime:
            clockSource.syncStarted();
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            Serial.println("[DEV MQTT] time sync requested");
            break;
        case DevSmokeAction::ConnectMqtt: {
            // DNS preflight: a Wi-Fi association surviving does not prove
            // the resolver is actually working (see CONTEXT.md's real-
            // hardware finding - a device stayed WL_CONNECTED for ~110 min
            // then lost DNS/TLS entirely). Resolve explicitly, once, before
            // ever attempting the heavier TLS/socket connect; never trust a
            // previous resolution as ongoing proof. The transport's own TLS
            // client still does its own internal resolution inside
            // transport.connect() below (out of our control, and not
            // reimplemented here) - this preflight is an additional, cheap,
            // explicit check so a DNS-specific failure is attributed
            // correctly instead of surfacing only as an opaque MQTT connect
            // failure.
            const bool networkReady = WiFi.dnsIP() != IPAddress() && WiFi.localIP() != IPAddress();
            IPAddress preflightResolved;
            const bool dnsOk = networkReady && WiFi.hostByName(INTERBRIDGE_DEV_AWS_ENDPOINT, preflightResolved) == 1;
            Serial.printf("[DEV MQTT] network preflight dns=%s\n", dnsOk ? "ok" : "failed");
            if (!dnsOk) {
                connectivity.networkPreflightFailed(now);
                Serial.printf("[DEV MQTT] connectivity_failures=%lu next DNS attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.consecutiveConnectivityFailures()),
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
                break;
            }

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
                Serial.printf("[DEV MQTT] connectivity_failures=%lu next MQTT attempt at_ms=%lu delay_ms=%lu\n",
                              static_cast<unsigned long>(connectivity.consecutiveConnectivityFailures()),
                              static_cast<unsigned long>(connectivity.retryAtMs()),
                              static_cast<unsigned long>(DevMqttSmokeState::millisUntil(connectivity.retryAtMs(), now)));
            }
            break;
        }
        case DevSmokeAction::RecoverWifi:
            // Authorized only after several consecutive DNS/TLS connectivity
            // failures with Wi-Fi already associated (see
            // DevMqttSmokeState::recordConnectivityFailure()) - a
            // conservative, cooldown-limited escalation, never a periodic
            // reboot and never ESP.restart().
            Serial.println("[DEV MQTT] wifi recovery requested");
            if (transport.isConnected()) transport.disconnect();
            subscribed = false;
            // wifioff=false keeps the radio on; eraseap=false keeps the
            // stored Wi-Fi credentials - only the current association is
            // dropped. The ordinary ConnectWifi/backoff flow (unchanged)
            // re-associates from here.
            WiFi.disconnect(false, false);
            Serial.printf("[DEV MQTT] wifi recovery cooldown until_ms=%lu\n",
                          static_cast<unsigned long>(connectivity.wifiRecoveryCooldownUntilMs()));
            break;
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
