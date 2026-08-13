#include "status_indicator.h"

namespace interbridge {

void Esp32StatusIndicator::show(ProvisioningIndication indication) {
    (void)indication;
    // TODO: not implemented - no LED GPIO/blink design chosen yet.
    // See CONTEXT.md > Open Questions.
}

void Esp32StatusIndicator::clear() {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
}

FakeStatusIndicator::FakeStatusIndicator()
    : hasIndication_(false), lastIndication_(ProvisioningIndication::ProvisioningAvailable), showCount_(0) {}

void FakeStatusIndicator::show(ProvisioningIndication indication) {
    hasIndication_ = true;
    lastIndication_ = indication;
    showCount_++;
}

void FakeStatusIndicator::clear() {
    hasIndication_ = false;
}

bool FakeStatusIndicator::hasIndication() const {
    return hasIndication_;
}

ProvisioningIndication FakeStatusIndicator::lastIndication() const {
    return lastIndication_;
}

int FakeStatusIndicator::showCount() const {
    return showCount_;
}

} // namespace interbridge
