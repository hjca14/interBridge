#pragma once

#include <cstdint>
#include <optional>

#include "si3050_bus.h"
#include "si3050_config.h"
#include "si3050_delay.h"
#include "si3050_pcm_clock.h"
#include "si3050_reset.h"

namespace interbridge {

// Runs the Si3050's documented electrical bring-up sequence and gates SPI
// access on it having completed. This is the hardware-independent,
// natively-testable core of the Phase 3A foundation - see
// docs/si3050-bringup.md for the full contract, its datasheet citations,
// and what is explicitly still out of scope (DAA/line register
// configuration, ring/off-hook/audio behavior).
//
// initialize() performs exactly these steps, in order, matching the
// Si3050 datasheet's electrical requirements (Table 6 "Switching
// Characteristics-General Inputs" and Section 5.31 "Communication
// Interface Mode Selection" - see si3050_timing.h for the precise
// citations):
//   1. CS deselected (high).
//   2. /RESET asserted (low).
//   3. SCLK held high (selects PCM/SPI mode, sampled by the Si3050 when
//      RESET is later released).
//   4. PCLK/FSYNC started.
//   5. Wait at least kSi3050MinPclkCyclesBeforeResetRelease PCLK cycles.
//   6. /RESET released (high).
//   7. Wait the PLL settle time (si3050PllSettleMicroseconds()).
//   8. isReady() becomes true - only now is transferRaw() permitted.
//
// initialize() never writes or reads a single Si3050 control register -
// DAA/line configuration remains explicitly out of scope for this PR.
// This class has no dependency on IHardwareIO or any door-actuation path
// by construction - it does not take one as a collaborator.
class Si3050Controller {
public:
    Si3050Controller(ISi3050Bus& bus, IPcmClock& clock, ISi3050Reset& reset, IDelayProvider& delay,
                     const Si3050Config& config = Si3050Config{});

    // Idempotent: a call while already ready() is a no-op, so it is safe
    // to call from a composition root without extra guard state.
    void initialize();

    bool isReady() const;

    // Raw single-byte SPI transfer, gated on isReady(): returns
    // std::nullopt without touching the bus at all if bring-up has not
    // completed yet. No register opcode/format is implied or validated
    // here - see docs/si3050-bringup.md for what remains future work.
    std::optional<uint8_t> transferRaw(uint8_t out);

private:
    ISi3050Bus& bus_;
    IPcmClock& clock_;
    ISi3050Reset& reset_;
    IDelayProvider& delay_;
    Si3050Config config_;
    bool ready_ = false;
};

} // namespace interbridge
