#pragma once

#include <cstdint>

namespace interbridge {

enum class DevSmokeState { WaitingForWifi, WaitingForDns, WaitingForTime, WaitingForMqtt, Online };
enum class DevSmokeAction { None, ConnectWifi, ResolveDns, ConfigureTime, ConnectMqtt };

// Hardware-independent coordinator. Deadlines use signed subtraction so they
// remain correct when the 32-bit Arduino millis() counter wraps.
class DevMqttSmokeState {
public:
    // ntpAttemptTimeoutMs: bounded, configurable ceiling on how long a single
    // ConfigureTime/SNTP attempt is trusted to still be "in flight" - see
    // update() and ntpAttemptInFlight().
    DevMqttSmokeState(uint32_t initialRetryMs = 1000, uint32_t maxRetryMs = 300000,
                      uint32_t ntpAttemptTimeoutMs = 15000);

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
    DevSmokeAction update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected);
    void dnsResult(uint32_t nowMs, bool success);
    void mqttResult(uint32_t nowMs, bool success);
    DevSmokeState state() const;
    uint32_t retryDelayMs() const;
    uint32_t retryAtMs() const;
    // Whether a ConfigureTime action's SNTP attempt is currently considered
    // in flight (issued, not yet resolved by timeValid or its own timeout).
    bool ntpAttemptInFlight() const;

    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs);

private:
    void enter(DevSmokeState state, uint32_t nowMs);
    void scheduleRetry(uint32_t nowMs);

    DevSmokeState state_;
    uint32_t initialRetryMs_;
    uint32_t maxRetryMs_;
    uint32_t ntpAttemptTimeoutMs_;
    uint32_t retryDelayMs_;
    uint32_t retryAtMs_;
    bool actionIssued_;
    bool ntpAttemptInFlight_ = false;
    uint32_t ntpAttemptDeadlineMs_ = 0;
};

} // namespace interbridge
