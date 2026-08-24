#pragma once

#include <cstdint>
#include <optional>

#include "si3050_bus.h"
#include "si3050_config.h"
#include "si3050_delay.h"
#include "si3050_pcm_clock.h"
#include "si3050_reset.h"

namespace interbridge {

// Outcome of Si3050Controller::initialize(). Ready is also returned by an
// idempotent call once already ready. InvalidConfig and ClockNotRunning
// are both fail-closed outcomes: /RESET stays asserted, isReady() stays
// false, and transferRaw() keeps returning std::nullopt - see the
// class-level contract below.
enum class Si3050InitResult { Ready, InvalidConfig, ClockNotRunning };

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
// citations) - but ONLY if both fail-closed gates below pass:
//   0. Si3050Config is validated first (pclkHz != 0 && fsyncHz != 0). An
//      invalid config never touches the bus or clock, asserts /RESET
//      (fail-closed) and returns InvalidConfig without dividing by zero
//      anywhere in the timing math.
//   1. CS deselected (high).
//   2. /RESET asserted (low).
//   3. SCLK held high (selects PCM/SPI mode, sampled by the Si3050 when
//      RESET is later released).
//   4. PCLK/FSYNC started via clock.start(). The result is NOT trusted
//      blindly: clock.isRunning() is checked immediately after. If it
//      reports false, bring-up stops here, fail-closed - /RESET stays
//      asserted, neither wait below runs, isReady() stays false, and
//      this call returns ClockNotRunning. A later initialize() call (once
//      the clock genuinely starts) retries the full sequence from
//      scratch - this outcome does NOT count as "already ready" and does
//      not set ready_. Esp32PcmClock's isRunning() always reports false
//      (see si3050_pcm_clock.h), so this class structurally refuses to
//      finish bring-up against that stub until real PCM clock generation
//      is implemented and verified.
//   5. Wait at least kSi3050MinPclkCyclesBeforeResetRelease PCLK cycles.
//   6. /RESET released (high).
//   7. Wait the PLL settle time (si3050PllSettleMicroseconds()).
//   8. isReady() becomes true and this call returns Ready - only now is
//      transferRaw() permitted.
//
// initialize() never writes or reads a single Si3050 control register -
// DAA/line configuration remains explicitly out of scope for this PR.
// This class has no dependency on IHardwareIO or any door-actuation path
// by construction - it does not take one as a collaborator.
class Si3050Controller {
public:
    Si3050Controller(ISi3050Bus& bus, IPcmClock& clock, ISi3050Reset& reset, IDelayProvider& delay,
                     const Si3050Config& config = Si3050Config{});

    // Idempotent only once genuinely Ready: a call while already ready()
    // is a no-op that returns Ready again. A prior InvalidConfig or
    // ClockNotRunning outcome is NOT "already ready" - the next call
    // re-attempts the full sequence from scratch.
    Si3050InitResult initialize();

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
