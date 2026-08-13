#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace interbridge {

// Source of random bytes used to generate 128-bit identifiers
// (event_id, device_id, internal command IDs) per
// docs/communication-protocol.md > Secure Random IDs. millis(),
// timestamps, incremental counters, or a MAC address alone must never be
// the sole source of uniqueness in production.
class IRandomSource {
public:
    virtual ~IRandomSource() = default;
    virtual void fill(uint8_t* buffer, size_t length) = 0;
};

// Real ESP32 implementation using the hardware RNG (esp_random(), backed
// by the SoC's true random number generator on ESP32/ESP32-C3 when RF is
// active). See CONTEXT.md for exactly what has/has not been validated on
// real hardware.
class Esp32RandomSource : public IRandomSource {
public:
    void fill(uint8_t* buffer, size_t length) override;
};

// Deterministic fake for native tests (xorshift32, seeded). Not
// cryptographically secure - test-only.
class FakeRandomSource : public IRandomSource {
public:
    explicit FakeRandomSource(uint32_t seed = 1);
    void fill(uint8_t* buffer, size_t length) override;

private:
    uint32_t state_;
};

// Formats 16 random bytes (128 bits) as "<prefix>-<32 lowercase hex
// chars>", e.g. generateHexId(rng, "evt") -> "evt-3f2a1c...".
std::string generateHexId(IRandomSource& random, const char* prefix);

// Generates a human-typeable decimal numeric code of `digitCount` digits
// (each '0'-'9'), e.g. for the 12-digit setup_code (see
// provisioning/device_identity.h). This is NOT a cryptographic
// identifier - see generateHexId() for those. A small per-digit modulo
// bias is acceptable here: the code only needs to be humanly memorable/
// typeable and correlated with a specific device by the backend, not to
// carry cryptographic security on its own (that comes from the BLE
// session security mode - see provisioning/ble_provisioning.h).
std::string generateNumericCode(IRandomSource& random, int digitCount);

// Formats a numeric code for human display, grouped in 4s:
// "482719362051" -> "4827 1936 2051".
std::string formatNumericCodeForDisplay(const std::string& code);

} // namespace interbridge
