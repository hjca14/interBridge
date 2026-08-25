#include "si3050_clock_probe_math.h"

#include <cmath>

namespace interbridge {

uint64_t combinePulseCount(uint32_t overflowCount, int32_t rawCount, int32_t hLimit) {
    if (hLimit <= 0) return 0;
    if (rawCount < 0 || rawCount > hLimit) return 0;
    return static_cast<uint64_t>(overflowCount) * static_cast<uint64_t>(hLimit) + static_cast<uint64_t>(rawCount);
}

double pulseFrequencyHz(uint64_t pulseEdges, uint64_t windowMicros) {
    if (windowMicros == 0) return 0.0;
    return static_cast<double>(pulseEdges) * 1000000.0 / static_cast<double>(windowMicros);
}

double pclkToFsyncRatio(uint64_t pclkRisingEdges, uint64_t fsyncRisingEdges) {
    if (fsyncRisingEdges == 0) return 0.0;
    return static_cast<double>(pclkRisingEdges) / static_cast<double>(fsyncRisingEdges);
}

ClockProbeWindowResult computeClockProbeWindowResult(uint64_t windowMicros, uint64_t pclkRisingEdges,
                                                      uint64_t fsyncRisingEdges) {
    ClockProbeWindowResult result;
    result.windowMicros = windowMicros;
    result.pclkRisingEdges = pclkRisingEdges;
    result.fsyncRisingEdges = fsyncRisingEdges;
    result.pclkHz = pulseFrequencyHz(pclkRisingEdges, windowMicros);
    result.fsyncHz = pulseFrequencyHz(fsyncRisingEdges, windowMicros);
    result.ratio = pclkToFsyncRatio(pclkRisingEdges, fsyncRisingEdges);
    return result;
}

void ClockProbeMinMaxTracker::observe(double value) {
    if (!std::isfinite(value)) return;
    if (!hasSample_) {
        min_ = value;
        max_ = value;
        hasSample_ = true;
        return;
    }
    if (value < min_) min_ = value;
    if (value > max_) max_ = value;
}

bool ClockProbeMinMaxTracker::hasSample() const {
    return hasSample_;
}

double ClockProbeMinMaxTracker::minValue() const {
    return min_;
}

double ClockProbeMinMaxTracker::maxValue() const {
    return max_;
}

} // namespace interbridge
