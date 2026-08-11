#include "intercom.h"

namespace interbridge {

Intercom::Intercom(IHardwareIO& hardware)
    : hardware_(hardware), lineDetector_(hardware) {}

std::optional<Event> Intercom::update() {
    return lineDetector_.update();
}

void Intercom::requestDoorOpen() {
    // TODO: pulse duration / latching behavior not yet defined.
    // See CONTEXT.md > Open Questions.
    hardware_.setDoorOutput(true);
}

} // namespace interbridge
