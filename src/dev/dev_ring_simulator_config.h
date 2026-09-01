#pragma once

#include <cstdint>

#include "../intercom/si3050/si3050_pins.h"

namespace interbridge {

// Phase 3B.8 bench-only DEV physical ring simulator GPIO. See
// docs/dev-ring-simulator.md > "Why GPIO4" and > "Bench test history" for
// the full wiring diagram and rationale.
//
// The validated bench board ("generic 4 MB ESP32-C3 Super Mini", see
// platformio.ini/CONTEXT.md) exposes only 15 GPIOs total: 0-10 and 18-21.
// Of those:
//   - GPIO2, GPIO8, GPIO9 are excluded here as BOOT/strapping pins.
//   - GPIO18/19 are the native USB D-/D+ pair (serial console) and stay
//     reserved.
//   - GPIO20/21 were the first choice (both are otherwise only
//     *documentation-reserved*, for a future physical config/reset
//     button and status LED - kSi3050ReservedPinButton/
//     kSi3050ReservedPinStatusLed - neither implemented in any current
//     code path). A real bench test wiring the button assembly then in
//     use to GPIO20 reliably correlated with Wi-Fi disconnecting
//     (WaitingForWifi -> ... -> Online with it unplugged; Wi-Fi dropped
//     again as soon as it was reconnected). That assembly was later found
//     to be electrically mismatched with the firmware's assumptions (a
//     Linker Button module, active-high, needs VCC/GND/SIG - not the
//     dry, active-low contact assumed at the time), so this correlation
//     does NOT establish GPIO20 itself, a silicon pin function, this
//     board's pinout, or RF/electrical interference as the cause - see
//     docs/dev-ring-simulator.md > Bench test history for the full,
//     corrected account. GPIO20/21 are avoided here out of caution from
//     that inconclusive result, not because a root cause was confirmed.
//   - GPIO0-8/10 are already committed to the real Si3050 wiring
//     (si3050_pins.h).
//
// With GPIO20/21 ruled out, the only remaining option on this board is a
// deliberate, explicit overlap with one real Si3050 pin. This is safe
// ONLY because this specific DEV-only environment
// (esp32-c3-dev-ring-simulator) never compiles or initializes any
// Si3050/RingDetector/PCM-clock code, and no Si3050 is physically
// attached to the board while this bench test runs (see "Scope and
// safety" in docs/dev-ring-simulator.md). GPIO4 (kSi3050PinPcmDrx, the
// Si3050's DRX line) is the explicitly approved overlap - never any
// other Si3050 pin, and never in any environment that does touch the
// real Si3050. This is NOT a production pin assignment and must be
// revisited once the Si3050 and the final board (with the real
// config/reset button) are integrated on the same physical unit - see
// CONTEXT.md > Open Questions.
constexpr uint8_t kDevRingButtonPin = kSi3050PinPcmDrx;

// Compile-time guard: this DEV-only pin must be exactly the one
// explicitly-approved Si3050 overlap above - GPIO4/kSi3050PinPcmDrx -
// never silently drift to any other Si3050 pin, the BOOT strap, or the
// USB D-/D+ pair. Changing kDevRingButtonPin to anything else requires
// deliberately updating this assertion too, which is the point: no
// future edit can silently pick an unreviewed pin.
static_assert(kDevRingButtonPin == kSi3050PinPcmDrx,
              "DEV ring simulator button GPIO must be the one explicitly-approved Si3050 overlap "
              "(GPIO4/kSi3050PinPcmDrx) - see docs/dev-ring-simulator.md > Bench test history for why GPIO20/21 "
              "are avoided and why only this specific overlap is considered safe");
static_assert(kDevRingButtonPin != kSi3050ReservedPinBoot && kDevRingButtonPin != kSi3050ReservedPinUsbDMinus &&
                  kDevRingButtonPin != kSi3050ReservedPinUsbDPlus,
              "DEV ring simulator button GPIO must never collide with BOOT/USB pins");

// This pass (call-session simulator): a second bench-only momentary input
// simulates the *end* of the same simulated call (RING_ENDED), correlated
// with the RING_DETECTED produced by kDevRingButtonPin above via a shared
// call_id - see docs/dev-ring-simulator.md > "Call session state machine".
//
// The same pin-survey constraints from kDevRingButtonPin's own comment
// apply unchanged: this board exposes only GPIO 0-10 and 18-21; 2/8/9 are
// BOOT/strapping, 18/19 are the native USB D-/D+ serial console pair, and
// 20/21 remain avoided per the GPIO20 Wi-Fi-drop investigation recorded in
// "Bench test history" (inconclusive root cause, avoided out of caution,
// not because GPIO20/21 themselves were confirmed at fault). That leaves
// the same class of choice as GPIO4 did: a deliberate, explicit overlap
// with one more real Si3050 pin.
//
// The chosen pin is GPIO3 (kSi3050PinPcmDtx, the Si3050's DTX line,
// Si3050 -> ESP). This is safe for exactly the same reason GPIO4/DRX is
// safe, and for no other reason: esp32-c3-dev-ring-simulator never
// compiles or initializes any Si3050/RingDetector/PCM-clock code (see
// "Scope and safety" in docs/dev-ring-simulator.md), and no Si3050 is
// physically attached to the board while this bench environment runs. It
// is NOT a production pin assignment, does not authorize simultaneous
// button/DTX use, and must be revisited once the Si3050 and the final
// board (with the real config/reset button) are integrated together -
// same caveat as kDevRingButtonPin above.
//
// This is a bench-only DEV-simulator addition, not a real end-of-call
// detector: nothing here reads the intercom line, DTMF, or any Si3050
// signal. See CONTEXT.md > Open Questions for the pending final-board
// pin decision.
constexpr uint8_t kDevRingEndButtonPin = kSi3050PinPcmDtx;

static_assert(kDevRingEndButtonPin == kSi3050PinPcmDtx,
              "DEV ring simulator end-of-call button GPIO must be the one explicitly-approved Si3050 overlap "
              "(GPIO3/kSi3050PinPcmDtx) - see this header's own comment above for why only this specific "
              "overlap is considered safe");
static_assert(kDevRingEndButtonPin != kSi3050ReservedPinBoot && kDevRingEndButtonPin != kSi3050ReservedPinUsbDMinus &&
                  kDevRingEndButtonPin != kSi3050ReservedPinUsbDPlus,
              "DEV ring simulator end-of-call button GPIO must never collide with BOOT/USB pins");
static_assert(kDevRingEndButtonPin != kDevRingButtonPin,
              "DEV ring simulator start (GPIO4) and end (GPIO3) buttons must never share the same pin");

} // namespace interbridge
