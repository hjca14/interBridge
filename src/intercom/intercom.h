#pragma once

#include <optional>

#include "../core/events.h"
#include "../hardware/gpio.h"
#include "line_detector.h"

namespace interbridge {

// High-level intercom abstraction. Business logic here must never depend
// on electrical details (GPIO numbers, voltage levels, timing) - those
// live behind IHardwareIO. This currently only wraps line detection;
// audio coordination will be added once the audio path is defined (see
// src/audio).
class Intercom {
public:
    explicit Intercom(IHardwareIO& hardware);

    // Polls the intercom line and returns an event if something changed.
    std::optional<Event> update();

    // Requests the door to be opened. The actuation mechanism (relay
    // type, pulse duration, latching behavior) is not defined yet; this
    // currently just drives the output on and leaves timing to the
    // caller. See CONTEXT.md > Open Questions.
    void requestDoorOpen();

private:
    IHardwareIO& hardware_;
    LineDetector lineDetector_;
};

} // namespace interbridge
