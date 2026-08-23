#pragma once

#include <cstdint>

namespace interbridge {

enum class DevSmokeState { WaitingForWifi, WaitingForDns, WaitingForTime, WaitingForMqtt, Online };
enum class DevSmokeAction { None, ConnectWifi, ResolveDns, ConfigureTime, ConnectMqtt, RecoverWifi };

// Hardware-independent coordinator. Deadlines use signed subtraction so they
// remain correct when the 32-bit Arduino millis() counter wraps.
class DevMqttSmokeState {
public:
    // ntpAttemptTimeoutMs: bounded, configurable ceiling on how long a single
    // ConfigureTime/SNTP attempt is trusted to still be "in flight" - see
    // update() and ntpAttemptInFlight().
    // wifiRecoveryThreshold: consecutive DNS/TLS connectivity failures (with
    // Wi-Fi already associated) before a single Wi-Fi interface recovery
    // (RecoverWifi) is authorized - see networkPreflightFailed()/
    // mqttResult() and consecutiveConnectivityFailures(). Deliberately
    // conservative: a real-hardware run went ~110 minutes online before a
    // transient DNS/TLS outage occurred, so this must not fire on a single
    // blip.
    // wifiRecoveryCooldownMs: minimum time between two Wi-Fi recoveries, so
    // a long-running local network/ISP outage does not keep bouncing the
    // radio - see wifiRecoveryCooldownActive()/wifiRecoveryCooldownUntilMs().
    DevMqttSmokeState(uint32_t initialRetryMs = 1000, uint32_t maxRetryMs = 300000,
                      uint32_t ntpAttemptTimeoutMs = 15000,
                      uint32_t wifiRecoveryThreshold = 3,
                      uint32_t wifiRecoveryCooldownMs = 600000);

    // Do not gate this on sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS
    // from the caller: on real hardware that status can stay reset/idle for a
    // while after configTime() is called, so a caller-supplied "in progress"
    // flag is not reliable as the sole guard. Instead, once a ConfigureTime
    // action is issued, this class tracks that attempt as in flight itself
    // (see ntpAttemptInFlight()) until either timeValid becomes true (the
    // real completion signal, via the caller observing its own sync-complete
    // callback) or ntpAttemptTimeoutMs elapses without that happening -
    // whichever comes first. While in flight, ConfigureTime is never
    // reissued, even past the ordinary backoff deadline. On timeout, the
    // attempt is treated as failed and exactly one fresh retry is allowed
    // once the ordinary backoff deadline is also reached.
    //
    // May return RecoverWifi instead of the state's ordinary next action if
    // enough consecutive connectivity failures have accumulated - see
    // networkPreflightFailed()/mqttResult(). The caller must tear its
    // transport down, call WiFi.disconnect() without erasing credentials,
    // and then let the ordinary ConnectWifi/backoff flow (unchanged) bring
    // the interface back up.
    DevSmokeAction update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected);
    // Explicit DNS resolution during the initial WaitingForDns bootstrap
    // stage (before NTP). success=false counts as a connectivity failure -
    // see consecutiveConnectivityFailures().
    void dnsResult(uint32_t nowMs, bool success);
    // Reports that the caller's own DNS preflight check - performed
    // immediately before attempting the actual TLS/socket MQTT connect, on
    // every ConnectMqtt action - failed. The caller must not attempt
    // transport.connect() when this is called. Transitions back to
    // WaitingForDns (that stage's own backoff, not the MQTT connect
    // backoff) and counts as a connectivity failure. Never call this during
    // the initial WaitingForDns bootstrap stage - use dnsResult() there.
    void networkPreflightFailed(uint32_t nowMs);
    // Reports the outcome of the actual TLS/socket MQTT connect (+
    // subscribe) attempt - only ever called after a successful preflight.
    // success=true (a full connect+subscribe) clears the connectivity
    // failure counter and any active Wi-Fi recovery cooldown; success=false
    // counts as a connectivity failure.
    void mqttResult(uint32_t nowMs, bool success);
    DevSmokeState state() const;
    uint32_t retryDelayMs() const;
    uint32_t retryAtMs() const;
    // Whether a ConfigureTime action's SNTP attempt is currently considered
    // in flight (issued, not yet resolved by timeValid or its own timeout).
    bool ntpAttemptInFlight() const;
    // Consecutive DNS-preflight/bootstrap or TLS/socket connectivity
    // failures since the last full MQTT connect+subscribe success. Publish
    // failures are never counted here - RemoteCommandProcessor/
    // Esp32AwsIotTransport already have their own outbox/session-
    // invalidation flow for those, independent of this interface-level
    // recovery ladder.
    uint32_t consecutiveConnectivityFailures() const;
    bool wifiRecoveryCooldownActive() const;
    uint32_t wifiRecoveryCooldownUntilMs() const;

    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs);

private:
    void enter(DevSmokeState state, uint32_t nowMs);
    void scheduleRetry(uint32_t nowMs);
    void recordConnectivityFailure(uint32_t nowMs);

    DevSmokeState state_;
    uint32_t initialRetryMs_;
    uint32_t maxRetryMs_;
    uint32_t ntpAttemptTimeoutMs_;
    uint32_t retryDelayMs_;
    uint32_t retryAtMs_;
    bool actionIssued_;
    bool ntpAttemptInFlight_ = false;
    uint32_t ntpAttemptDeadlineMs_ = 0;

    uint32_t wifiRecoveryThreshold_;
    uint32_t wifiRecoveryCooldownMs_;
    uint32_t consecutiveConnectivityFailures_ = 0;
    bool wifiRecoveryCooldownActive_ = false;
    uint32_t wifiRecoveryCooldownUntilMs_ = 0;
    bool wifiRecoveryRequested_ = false;
};

} // namespace interbridge
