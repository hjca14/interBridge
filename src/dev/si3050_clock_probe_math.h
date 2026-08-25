#pragma once

#include <cstdint>

namespace interbridge {

// Hardware-independent math for the Phase 3B.1 clock probe bench
// experiment (src/dev/si3050_clock_probe_meter_main.cpp). This is NOT
// part of the Si3050 foundation itself - it only measures whether an
// ESP32-C3 can generate the target PCLK/FSYNC clocks in hardware, and
// converts hardware pulse-counter samples into frequencies/ratios. No
// Wi-Fi, MQTT, or door-actuation dependency.

// The classic ESP32's PCNT hardware counter register is only 16-bit
// signed (see pcnt_config_t::counter_h_lim in driver/pcnt.h, an int16_t).
// At the Si3050's target PCLK of 2.048 MHz, a useful ~1 s measurement
// window produces far more edges than that register can hold, so the
// meter firmware configures a PCNT_EVT_H_LIM watchpoint well inside the
// int16_t range and an ISR that increments a software overflow counter
// (and resets the hardware counter to 0) every time the watchpoint
// fires. This is the H_LIM value the meter firmware's PCNT configuration
// must actually use - kept here so it stays in sync with the math below
// instead of being duplicated as a second magic number.
constexpr int32_t kClockProbePcntHighLimit = 30000;

// The Si3050's PCM/SPI-mode clock target - the mode InterBridge actually
// plans to use (SPI for control, PCM for audio), per the datasheet's
// Clock Generation and PCM Highway sections: PCLK must be synchronous to
// an 8 kHz FSYNC, and 1.024 MHz is a valid PCLK rate in this mode
// (1,024,000 / 8,000 = 128 PCLK cycles per frame = 16 timeslots of 8
// bits). This is NOT the same as the Si3050's GCI mode, which requires
// PCLK = 2.048 or 4.096 MHz and multiplexes control with data on the same
// highway - GCI is not the mode InterBridge plans to use. See
// docs/si3050-clock-probe.md for the full distinction.
constexpr uint32_t kPcmSpiTargetPclkHz = 1024000;
constexpr uint32_t kPcmSpiTargetFsyncHz = 8000;
constexpr uint32_t kPcmSpiTargetRatio = 128;

// Combines a hardware pulse counter's periodic overflow count with its
// current raw value into a single, unbounded pulse total since the last
// reset.
//
// overflowCount: number of times the hardware counter reached hLimit and
//   was reset back to 0 (e.g. incremented by a PCNT H_LIM interrupt).
// rawCount: the hardware counter's current value, expected in [0, hLimit].
// hLimit: the H_LIM threshold the hardware counter was configured with.
//
// Returns 0 for a pathological/inconsistent input (hLimit <= 0, or
// rawCount outside [0, hLimit]) rather than producing a nonsensical
// total - this can legitimately happen if the raw counter is sampled
// without excluding the overflow ISR (a torn read), so callers must
// treat 0 here as "sample was inconsistent, discard this window", not
// "zero pulses were seen".
uint64_t combinePulseCount(uint32_t overflowCount, int32_t rawCount, int32_t hLimit);

// Frequency implied by a pulse count over a real, measured window
// duration. Returns 0.0 if windowMicros is 0 (would otherwise divide by
// zero) - a degenerate window, never a valid measurement.
double pulseFrequencyHz(uint64_t pulseEdges, uint64_t windowMicros);

// PCLK:FSYNC ratio for rising-edge counts taken over the same window -
// computed directly from the two edge counts (not from two already-
// rounded frequencies), so it is exact regardless of window duration or
// jitter in when the window boundary was sampled. Returns 0.0 if
// fsyncRisingEdges is 0 (would otherwise divide by zero). Both counts
// must be rising-edge-only counts (see the meter's PCNT configuration,
// pos_mode=PCNT_COUNT_INC/neg_mode=PCNT_COUNT_DIS on both units) -
// counting both edges of either signal would silently double it.
double pclkToFsyncRatio(uint64_t pclkRisingEdges, uint64_t fsyncRisingEdges);

// One reporting window's full result, as printed by the meter firmware
// roughly once per second. The edge counts are rising-edge-only (see
// pclkToFsyncRatio() above) - never a count of both edges.
struct ClockProbeWindowResult {
    uint64_t windowMicros = 0;
    uint64_t pclkRisingEdges = 0;
    uint64_t fsyncRisingEdges = 0;
    double pclkHz = 0.0;
    double fsyncHz = 0.0;
    double ratio = 0.0;
};

ClockProbeWindowResult computeClockProbeWindowResult(uint64_t windowMicros, uint64_t pclkRisingEdges,
                                                      uint64_t fsyncRisingEdges);

// Running minimum/maximum tracker for one bench session's worth of
// per-window measurements (pclkHz, fsyncHz, or ratio, tracked
// separately). Non-finite samples (NaN/inf, which a degenerate window
// could otherwise produce upstream) are ignored rather than corrupting
// the running min/max.
class ClockProbeMinMaxTracker {
public:
    void observe(double value);

    bool hasSample() const;
    double minValue() const;
    double maxValue() const;

private:
    bool hasSample_ = false;
    double min_ = 0.0;
    double max_ = 0.0;
};

} // namespace interbridge
