#include "random_id.h"

#include <cstdio>

#ifdef ARDUINO
#include <esp_system.h>
#endif

namespace interbridge {

void Esp32RandomSource::fill(uint8_t* buffer, size_t length) {
#ifdef ARDUINO
    size_t i = 0;
    while (i < length) {
        uint32_t word = esp_random();
        size_t chunk = length - i < 4 ? length - i : 4;
        for (size_t b = 0; b < chunk; b++) {
            buffer[i + b] = static_cast<uint8_t>(word >> (8 * b));
        }
        i += chunk;
    }
#else
    // Not meaningful off-target; native builds must use FakeRandomSource.
    for (size_t i = 0; i < length; i++) {
        buffer[i] = 0;
    }
#endif
}

FakeRandomSource::FakeRandomSource(uint32_t seed) : state_(seed == 0 ? 1 : seed) {}

void FakeRandomSource::fill(uint8_t* buffer, size_t length) {
    for (size_t i = 0; i < length; i++) {
        // xorshift32 - deterministic, test-only, NOT cryptographically secure.
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        buffer[i] = static_cast<uint8_t>(state_ & 0xFF);
    }
}

std::string generateHexId(IRandomSource& random, const char* prefix) {
    uint8_t bytes[16];
    random.fill(bytes, sizeof(bytes));

    char hex[33];
    for (int i = 0; i < 16; i++) {
        std::snprintf(&hex[i * 2], 3, "%02x", bytes[i]);
    }

    std::string id(prefix);
    id += "-";
    id.append(hex, 32);
    return id;
}

} // namespace interbridge
