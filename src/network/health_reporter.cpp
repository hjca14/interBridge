#include "health_reporter.h"

namespace interbridge {

HealthReporter::HealthReporter(uint32_t intervalMs)
    : intervalMs_(intervalMs), lastPublishMs_(0), hasPublished_(false), forced_(false) {}

bool HealthReporter::isDue(uint32_t nowMs) {
    if (forced_) {
        forced_ = false;
        lastPublishMs_ = nowMs;
        hasPublished_ = true;
        return true;
    }

    if (!hasPublished_) {
        lastPublishMs_ = nowMs;
        hasPublished_ = true;
        return true;
    }

    if (nowMs - lastPublishMs_ >= intervalMs_) {
        lastPublishMs_ = nowMs;
        return true;
    }

    return false;
}

void HealthReporter::forceNextPublish() {
    forced_ = true;
}

} // namespace interbridge
