#include "si3050_clock_probe_meter_bringup.h"

namespace interbridge {

bool isPcntIsrServiceReady(ClockProbeEspErr result) {
    return result == kClockProbeEspOk || result == kClockProbeEspErrInvalidState;
}

void PcntBringupTracker::record(const char* stepName, ClockProbeEspErr result, bool succeeded) {
    if (hasFailed_) return; // only the first (root-cause) failure is kept
    if (succeeded) return;
    hasFailed_ = true;
    failedStepName_ = stepName;
    failedStepResult_ = result;
}

bool PcntBringupTracker::hasFailed() const {
    return hasFailed_;
}

const char* PcntBringupTracker::failedStepName() const {
    return failedStepName_;
}

ClockProbeEspErr PcntBringupTracker::failedStepResult() const {
    return failedStepResult_;
}

} // namespace interbridge
