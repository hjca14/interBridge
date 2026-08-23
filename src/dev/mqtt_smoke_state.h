#pragma once

#include <cstdint>

namespace interbridge {

enum class DevSmokeState { WaitingForWifi, WaitingForDns, WaitingForTime, WaitingForMqtt, Online };
enum class DevSmokeAction { None, ConnectWifi, ResolveDns, ConfigureTime, ConnectMqtt };

// Hardware-independent coordinator. Deadlines use signed subtraction so they
// remain correct when the 32-bit Arduino millis() counter wraps.
class DevMqttSmokeState {
public:
    DevMqttSmokeState(uint32_t initialRetryMs = 1000, uint32_t maxRetryMs = 300000);

    // timeSyncInProgress: whether a previously issued ConfigureTime action's
    // SNTP attempt may still be in flight (e.g. sntp_get_sync_status() ==
    // SNTP_SYNC_STATUS_IN_PROGRESS). While true, ConfigureTime is never
    // reissued even if the backoff deadline has elapsed - calling
    // configTime() again would restart that attempt instead of letting it
    // finish, which can starve NTP sync entirely once the backoff interval
    // becomes shorter than a real synchronization round trip. Defaults to
    // false so existing callers/tests that don't track this are unaffected.
    DevSmokeAction update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected,
                          bool timeSyncInProgress = false);
    void dnsResult(uint32_t nowMs, bool success);
    void mqttResult(uint32_t nowMs, bool success);
    DevSmokeState state() const;
    uint32_t retryDelayMs() const;
    uint32_t retryAtMs() const;

    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs);

private:
    void enter(DevSmokeState state, uint32_t nowMs);
    void scheduleRetry(uint32_t nowMs);

    DevSmokeState state_;
    uint32_t initialRetryMs_;
    uint32_t maxRetryMs_;
    uint32_t retryDelayMs_;
    uint32_t retryAtMs_;
    bool actionIssued_;
};

} // namespace interbridge
