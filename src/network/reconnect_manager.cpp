#include "reconnect_manager.h"

#include <algorithm>
#include <cstdint>

namespace interbridge {

ReconnectManager::ReconnectManager(IRandomSource& random, uint32_t initialDelayMs, uint32_t maxDelayMs)
    : random_(random), initialDelayMs_(initialDelayMs), maxDelayMs_(maxDelayMs), attempt_(0) {}

uint32_t ReconnectManager::nextDelayMs() {
    // Clamp the exponent so initialDelayMs_ * 2^attempt_ cannot overflow
    // before being clamped to maxDelayMs_.
    int cappedAttempt = std::min(attempt_, 31);
    uint64_t exponential = static_cast<uint64_t>(initialDelayMs_) << cappedAttempt;
    uint32_t cap = static_cast<uint32_t>(std::min<uint64_t>(exponential, maxDelayMs_));

    uint8_t bytes[4];
    random_.fill(bytes, sizeof(bytes));
    uint32_t raw = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
                   (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);

    attempt_++;
    return raw % (cap + 1);
}

void ReconnectManager::reset() {
    attempt_ = 0;
}

int ReconnectManager::attempt() const {
    return attempt_;
}

} // namespace interbridge
