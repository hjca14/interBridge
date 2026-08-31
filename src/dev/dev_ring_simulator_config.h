#pragma once

#include <cstdint>

#include "../intercom/si3050/si3050_pins.h"

namespace interbridge {

// Phase 3B.8 bench-only DEV physical ring simulator GPIO. See
// docs/dev-ring-simulator.md for the full wiring diagram and rationale,
// including the real-hardware evidence that ruled out GPIO20 (see "Real
// bench observation: GPIO20 causes Wi-Fi to disconnect").
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
//     code path). A real bench test wiring the button to GPIO20 reliably
//     disconnected Wi-Fi association (WaitingForWifi -> ... -> Online
//     with the button unplugged; Wi-Fi dropped again as soon as it was
//     reconnected to GPIO20) - see docs/dev-ring-simulator.md. The exact
//     physical cause (an alternate silicon pin function, this specific
//     button/wiring's mounting, this board's particular pinout, or
//     electrical/RF interference) has NOT been isolated - only the
//     correlation itself was reproduced, which is enough to abandon
//     GPIO20 (and, out of caution, GPIO21) for this bench rig without
//     waiting for a root cause.
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
              "(GPIO4/kSi3050PinPcmDrx) - see docs/dev-ring-simulator.md for why GPIO20/21 were ruled out on real "
              "hardware and why only this specific overlap is considered safe");
static_assert(kDevRingButtonPin != kSi3050ReservedPinBoot && kDevRingButtonPin != kSi3050ReservedPinUsbDMinus &&
                  kDevRingButtonPin != kSi3050ReservedPinUsbDPlus,
              "DEV ring simulator button GPIO must never collide with BOOT/USB pins");

} // namespace interbridge
