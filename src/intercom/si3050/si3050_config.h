#pragma once

#include <cstdint>

namespace interbridge {

// PCM clock targets for Rev A - see
// https://github.com/hjca14/interhardware/blob/main/docs/decisions/INTERBRIDGE_GPIO_MAP.md
// and the Si3050 datasheet's Section 5.30 "Clock Generation": PCLK must
// run at one of a fixed set of rates synchronous to an 8 kHz FSYNC. Only
// PCLK/FSYNC generation targets live here - no DAA/line register content
// (that remains explicitly out of scope for this foundation, see
// Si3050Controller).
struct Si3050Config {
    uint32_t pclkHz = 2048000;
    uint32_t fsyncHz = 8000;
};

} // namespace interbridge
