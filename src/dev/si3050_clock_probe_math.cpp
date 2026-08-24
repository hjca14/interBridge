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

double pclkToFsyncRatio(uint64_t pclkEdges, uint64_t fsyncEdges) {
    if (fsyncEdges == 0) return 0.0;
    return static_cast<double>(pclkEdges) / static_cast<double>(fsyncEdges);
}

ClockProbeWindowResult computeClockProbeWindowResult(uint64_t windowMicros, uint64_t pclkEdges, uint64_t fsyncEdges) {
    ClockProbeWindowResult result;
    result.windowMicros = windowMicros;
    result.pclkEdges = pclkEdges;
    result.fsyncEdges = fsyncEdges;
    result.pclkHz = pulseFrequencyHz(pclkEdges, windowMicros);
    result.fsyncHz = pulseFrequencyHz(fsyncEdges, windowMicros);
    result.ratio = pclkToFsyncRatio(pclkEdges, fsyncEdges);
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
