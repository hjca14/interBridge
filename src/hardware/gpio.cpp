#include "gpio.h"

namespace interbridge {

Esp32GpioHardware::Esp32GpioHardware() {
    // TODO: configure pin modes once the intercom circuit is defined.
    // See CONTEXT.md > Open Questions.
}

bool Esp32GpioHardware::readLineState() {
    // TODO: not implemented. GPIO mapping and the electrical criteria for
    // detecting off-hook/ring have not been defined yet.
    // See CONTEXT.md > Open Questions.
    return false;
}

void Esp32GpioHardware::setDoorOutput(bool enabled) {
    (void)enabled;
    // TODO: not implemented. The door release circuit (relay/transistor,
    // pulse duration, active level) has not been defined yet.
    // See CONTEXT.md > Open Questions.
}

} // namespace interbridge
