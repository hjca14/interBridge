#pragma once

#include <cstdint>

namespace interbridge {

// Time abstraction. Every module that reasons about time (reconnect
// backoff, command expiry, heartbeat/health cadence, button hold
// duration, dedup/outbox TTLs) takes time through this interface instead
// of calling millis()/time() directly, so native tests can inject a
// deterministic fake clock and never sleep for real.
class IClock {
public:
    virtual ~IClock() = default;

    // Monotonic milliseconds since boot. Not wall-clock time, never goes
    // backwards, wraps per the underlying millis() semantics.
    virtual uint32_t monotonicMs() const = 0;

    // Whether wall-clock time is currently trustworthy (e.g. NTP sync
    // completed). NTP/time sync is NOT implemented yet - see CONTEXT.md >
    // Open Questions. Command time-safety validation (see
    // protocol/command_handler.*) must refuse to trust
    // issued_at/expires_at before this is true.
    virtual bool hasValidTime() const = 0;

    // Wall-clock Unix time in seconds. Only meaningful when
    // hasValidTime() is true.
    virtual int64_t unixTimeSeconds() const = 0;
};

// Real ESP32 implementation. monotonicMs() can be implemented today via
// Arduino millis(). hasValidTime()/unixTimeSeconds() require NTP/time
// sync, which is not implemented yet - they are stubs that always report
// "no valid time" until that is wired up.
class Esp32Clock : public IClock {
public:
    uint32_t monotonicMs() const override;
    bool hasValidTime() const override;
    int64_t unixTimeSeconds() const override;
};

// Deterministic fake clock for native tests. Both monotonic and wall
// time are controlled explicitly by the test.
class FakeClock : public IClock {
public:
    FakeClock();

    void setMonotonicMs(uint32_t ms);
    void advanceMs(uint32_t deltaMs);

    void setUnixTimeSeconds(int64_t seconds); // also marks hasValidTime() true
    void invalidateTime();                     // marks hasValidTime() false again

    uint32_t monotonicMs() const override;
    bool hasValidTime() const override;
    int64_t unixTimeSeconds() const override;

private:
    uint32_t monotonicMs_;
    bool hasValidTime_;
    int64_t unixTimeSeconds_;
};

} // namespace interbridge
