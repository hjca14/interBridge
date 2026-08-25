#pragma once

#include <cstdint>

namespace interbridge {

// Pure arithmetic describing the TDM slot geometry the generator ASKS
// the I2S driver for (src/dev/si3050_clock_probe_generator_main.cpp).
// A real bench test of the generator's original geometry, totalChan=16,
// bitsPerSample=16 (configuredTdmRatio()=256, matching the Si3050's
// then-assumed GCI-mode-style PCLK:FSYNC target) measured an actual
// ratio of ~64 on real hardware instead - see
// docs/si3050-clock-probe.md's "Real bench observation: generator does
// not reach the target ratio" section for the full investigation,
// including a later cross-reference against the matching upstream
// ESP-IDF source (this framework package itself ships only a
// precompiled `driver/i2s` library, no source) that found the driver's
// own documented clock formula does not predict that measurement
// either. The generator now requests totalChan=16, bitsPerSample=8
// (configuredTdmRatio()=128) instead, matching the Si3050's PCM/SPI-mode
// PCM Highway geometry - **a real bench retest of this exact geometry
// confirmed it on physical hardware** (`pclk_hz ~= 1,024,100`, `ratio ~=
// 127.98`-`128.00` across stable windows) - see
// docs/si3050-clock-probe.md's "Real bench observation: 16 x 8 slot
// geometry reaches the PCM/SPI target" section. This confirms the clock
// signal only: no real Si3050 has been connected, and this geometry is
// not yet integrated into `Esp32PcmClock` or any real firmware path.

// The BCLK:WS ratio the generator's i2s_config_t REQUESTS - the number
// of PCLK cycles per FSYNC frame the driver is told to produce. This is
// exactly what the generator logs as "requested_clocks_per_frame" -
// never claimed as the real, measured ratio (the separate meter board's
// measurement is what confirms that - see the caveat above).
constexpr uint32_t configuredTdmRatio(uint32_t totalChan, uint32_t bitsPerSample) {
    return totalChan * bitsPerSample;
}

// The BCLK the generator REQUESTS, per the driver's own documented
// formula (driver/i2s.h, i2s_set_sample_rates(): "bit_clock = rate *
// (number of channels) * bits_per_sample"). Real hardware measurement
// showed this formula was NOT honored as-is for the previous, 16 x 16
// geometry, but WAS confirmed close to accurate for the current, 16 x 8
// geometry - see the caveat above.
constexpr uint32_t configuredBclkHz(uint32_t sampleRateHz, uint32_t totalChan, uint32_t bitsPerSample) {
    return sampleRateHz * configuredTdmRatio(totalChan, bitsPerSample);
}

} // namespace interbridge
