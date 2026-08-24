#pragma once

#include <cstdint>

namespace interbridge {

// Pure arithmetic describing the TDM slot geometry the generator ASKS
// the I2S driver for (src/dev/si3050_clock_probe_generator_main.cpp) -
// this is NOT a measurement and NOT a guarantee the driver/hardware
// actually delivers it. A real bench test of totalChan=16,
// bitsPerSample=16 (configuredTdmRatio()=256, matching the Si3050's
// PCLK:FSYNC target exactly) measured an actual ratio of ~64 on real
// hardware instead - see docs/si3050-clock-probe.md's "Real bench
// observation: generator does not reach the target ratio" section for
// the full investigation. Only a precompiled `driver/i2s` library (no
// source) is available in this framework package, so the discrepancy's
// root cause could not be fully confirmed from source.

// The BCLK:WS ratio the generator's i2s_config_t/i2s_set_clk() REQUESTS
// - the number of PCLK cycles per FSYNC frame the driver is told to
// produce. This is exactly what the generator logs as
// "requested_ratio" - never claimed as the real, measured ratio, which
// only the separate meter board can determine.
constexpr uint32_t configuredTdmRatio(uint32_t totalChan, uint32_t bitsPerSample) {
    return totalChan * bitsPerSample;
}

// The BCLK the generator REQUESTS, per the driver's own documented
// formula (driver/i2s.h, i2s_set_sample_rates(): "bit_clock = rate *
// (number of channels) * bits_per_sample"). Real hardware measurement
// has shown this formula is not honored as-is for TDM mode on this
// chip/driver/framework combination - see the caveat above.
constexpr uint32_t configuredBclkHz(uint32_t sampleRateHz, uint32_t totalChan, uint32_t bitsPerSample) {
    return sampleRateHz * configuredTdmRatio(totalChan, bitsPerSample);
}

} // namespace interbridge
