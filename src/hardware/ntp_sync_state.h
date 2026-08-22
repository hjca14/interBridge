#pragma once

#include <cstdint>

namespace interbridge {

// Explicit NTP-completion gate. A plausible epoch alone is not proof that the
// current synchronization attempt completed (it may be a stale RTC value).
class NtpSyncState {
public:
    explicit NtpSyncState(uint32_t settleMs = 1000) : settleMs_(settleMs) {}
    void synchronizationStarted() { completed_ = false; }
    void synchronizationCompleted(uint32_t nowMs) { completed_ = true; completedAtMs_ = nowMs; }
    bool isTrustworthy(uint32_t nowMs, bool synchronizationInProgress) const {
        return completed_ && !synchronizationInProgress &&
               static_cast<uint32_t>(nowMs - completedAtMs_) >= settleMs_;
    }
private:
    uint32_t settleMs_;
    uint32_t completedAtMs_ = 0;
    bool completed_ = false;
};

} // namespace interbridge
