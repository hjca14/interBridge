#include "line_detector.h"

namespace interbridge {

LineDetector::LineDetector(IHardwareIO& hardware)
    : hardware_(hardware), lastLineState_(false), initialized_(false) {}

std::optional<Event> LineDetector::update() {
    bool current = hardware_.readLineState();

    if (!initialized_) {
        lastLineState_ = current;
        initialized_ = true;
        return std::nullopt;
    }

    if (current == lastLineState_) {
        return std::nullopt;
    }

    lastLineState_ = current;
    return Event{current ? EventType::OffHook : EventType::OnHook};
}

} // namespace interbridge
