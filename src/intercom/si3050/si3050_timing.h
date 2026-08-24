#pragma once

#include <cstdint>

namespace interbridge {

// Electrical bring-up timing sourced from the Si3050 datasheet present in
// this repository (docs/Si3050-11-18-19.pdf - Skyworks "Si3050+Si3011/18/
// 19", Rev. 1.5, August 24 2021). Nothing here is guessed:
//   - Table 6 "Switching Characteristics-General Inputs" (page 10):
//     "PCLK Before RESET" (tmr) >= 10 cycles; "CS, SCLK Before RESET"
//     (tmxr) >= 20 ns; "RESET Pulse Width" (trl) >= the greater of 250 ns
//     or 10 PCLK cycle times.
//   - Section 5.30 "Clock Generation" (page 39): the PLL clock
//     synthesizer settling time is given by Tsettle = 64 / FPCLK; Section
//     5.3 "Initialization" separately states this is "less than 1 ms from
//     the application of PCLK", consistent with the formula at any valid
//     PCLK rate.
//   - Section 5.31 "Communication Interface Mode Selection" / Table 20
//     (page 39): the state of SCLK at the moment RESET is sampled selects
//     PCM/SPI mode (SCLK=1) vs. GCI mode - this is why SCLK must be held
//     high before RESET is released.
// No register-level (DAA/line) configuration is sourced or implemented
// here - see Si3050Controller's own scope note.

constexpr uint32_t kSi3050MinPclkCyclesBeforeResetRelease = 10; // tmr

// Ceiling of (cycles / pclkHz) in whole microseconds - the minimum
// blocking wait that guarantees at least `cycles` PCLK periods have
// elapsed, without needing to count real clock edges in software.
constexpr uint32_t si3050CyclesToMicroseconds(uint32_t cycles, uint32_t pclkHz) {
    return static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000000ULL + pclkHz - 1) / pclkHz);
}

// Tsettle = 64 / FPCLK (Section 5.30), rounded up to a whole microsecond.
constexpr uint32_t si3050PllSettleMicroseconds(uint32_t pclkHz) {
    return si3050CyclesToMicroseconds(64, pclkHz);
}

} // namespace interbridge
