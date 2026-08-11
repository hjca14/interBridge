#pragma once

#include <optional>

#include "../core/events.h"
#include "../hardware/gpio.h"

namespace interbridge {

// Detects OFF_HOOK / ON_HOOK transitions from the raw line state exposed
// by IHardwareIO.
//
// Ring detection is intentionally NOT implemented here: telling a ring
// signal apart from an off-hook condition from a single boolean line read
// requires characterizing the real intercom line electrically (e.g. a
// dedicated ring-detect signal, or sampling an AC ring waveform), which
// has not been done yet. See CONTEXT.md > Open Questions.
class LineDetector {
public:
    explicit LineDetector(IHardwareIO& hardware);

    // Polls the hardware and returns an event if the line state changed
    // since the last call. Returns std::nullopt on the first call (used
    // to establish a baseline) and whenever nothing changed.
    std::optional<Event> update();

private:
    IHardwareIO& hardware_;
    bool lastLineState_;
    bool initialized_;
};

} // namespace interbridge
