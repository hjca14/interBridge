#pragma once

#include <cstdint>

namespace interbridge {

// PCM clock targets for Rev A - see
// https://github.com/hjca14/interhardware/blob/main/docs/decisions/INTERBRIDGE_GPIO_MAP.md
// and the Si3050 datasheet's "Clock Generation" section (cited by title,
// not section number - numbering varies between datasheet revisions):
// PCLK must run at one of a fixed set of rates synchronous to an 8 kHz
// FSYNC. Only PCLK/FSYNC generation targets live here - no DAA/line
// register content (that remains explicitly out of scope for this
// foundation, see Si3050Controller).
//
// PCLK = 1.024 MHz, FSYNC = 8 kHz is the Si3050's PCM/SPI-mode PCM
// Highway geometry (16 timeslots x 8 bits/timeslot = 128 PCLK
// cycles/frame) - the mode InterBridge uses (SPI for control, PCM for
// audio), NOT the GCI mode's 2.048/4.096 MHz requirement. This exact
// 1.024 MHz/8 kHz/128 target was physically validated on real ESP32-C3
// hardware, measured externally by an independent PCNT-based meter
// board, in the Phase 3B.1 clock probe bench experiment - see
// docs/si3050-clock-probe.md's "Real bench observation: 16 x 8 slot
// geometry reaches the PCM/SPI target". Esp32PcmClock
// (si3050_pcm_clock.h) implements exactly this geometry.
struct Si3050Config {
    uint32_t pclkHz = 1024000;
    uint32_t fsyncHz = 8000;
};

} // namespace interbridge
