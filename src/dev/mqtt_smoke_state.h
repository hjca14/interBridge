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

    DevSmokeAction update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected);
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
