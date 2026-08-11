#pragma once

#include <cstdint>

#include "../core/random_id.h"

namespace interbridge {

constexpr uint32_t kReconnectInitialDelayMs = 1000;
constexpr uint32_t kReconnectMaxDelayMs = 300000;

// Non-blocking exponential backoff with full jitter for MQTT reconnect
// attempts, per docs/communication-protocol.md > Reconnect Manager:
//   delay = random(0, min(maxDelay, initialDelay * 2^attempt))
//
// This class never sleeps or blocks - it only computes the next delay.
// The caller (device transport / main loop) is responsible for timing
// the next attempt against its own clock.
class ReconnectManager {
public:
    explicit ReconnectManager(IRandomSource& random,
                               uint32_t initialDelayMs = kReconnectInitialDelayMs,
                               uint32_t maxDelayMs = kReconnectMaxDelayMs);

    // Call once when a connection attempt fails. Returns the delay (ms)
    // to wait before the next attempt and advances the backoff exponent.
    uint32_t nextDelayMs();

    // Call on a successful connection to reset backoff for the next time
    // a disconnect happens. On successful reconnect, the caller is also
    // responsible for: restoring subscriptions, resynchronizing the
    // Device Shadow, checking pending AWS IoT Jobs, and flushing the
    // event outbox - see device_transport.*.
    void reset();

    int attempt() const;

private:
    IRandomSource& random_;
    uint32_t initialDelayMs_;
    uint32_t maxDelayMs_;
    int attempt_;
};

} // namespace interbridge
