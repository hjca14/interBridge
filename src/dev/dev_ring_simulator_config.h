#pragma once

#include <cstdint>

#include "../intercom/si3050/si3050_pins.h"

namespace interbridge {

// Phase 3B.8 bench-only DEV physical ring simulator GPIO. See
// docs/dev-ring-simulator.md for the full wiring diagram and rationale.
//
// The validated bench board ("generic 4 MB ESP32-C3 Super Mini", see
// platformio.ini/CONTEXT.md) exposes only 15 GPIOs total: 0-10 and 18-21.
// Of those, si3050_pins.h already commits GPIO0-8/10 to the real Si3050
// wiring, GPIO9 is the BOOT/download-mode strap, and GPIO18/19 are the
// native USB D-/D+ pair used for the serial console (ARDUINO_USB_MODE=1 +
// ARDUINO_USB_CDC_ON_BOOT=1). That leaves only GPIO20/21, both of which
// are *documentation-reserved only* (kSi3050ReservedPinButton/
// kSi3050ReservedPinStatusLed) for a future physical config/reset button
// and status LED - neither is implemented in any current code path
// (Esp32ButtonInput/Esp32StatusIndicator remain unassigned stubs, see
// hardware/button.cpp / hardware/status_indicator.cpp).
//
// Explicit, user-approved decision for THIS DEV-only environment only:
// temporarily reuse GPIO20 for the bench ring-simulator button, scoped
// exclusively to esp32-c3-dev-ring-simulator. This is NOT a production
// pin assignment and must be revisited once the Si3050 and the final
// board/config-reset-button are integrated on the same physical unit -
// see CONTEXT.md > Open Questions.
constexpr uint8_t kDevRingButtonPin = 20;

// Compile-time guard: this DEV-only pin must never silently collide with
// a real Si3050 wire, the BOOT strap, or the USB D-/D+ pair. It is
// deliberately NOT checked against kSi3050ReservedPinButton/
// kSi3050ReservedPinStatusLed - reusing one of those two specific
// reserved-but-unimplemented pins is the documented, approved choice
// above, not an oversight.
static_assert(kDevRingButtonPin != kSi3050PinPclk && kDevRingButtonPin != kSi3050PinFsync &&
                  kDevRingButtonPin != kSi3050PinSpiMiso && kDevRingButtonPin != kSi3050PinPcmDtx &&
                  kDevRingButtonPin != kSi3050PinPcmDrx && kDevRingButtonPin != kSi3050PinReset &&
                  kDevRingButtonPin != kSi3050PinSpiSclk && kDevRingButtonPin != kSi3050PinSpiMosi &&
                  kDevRingButtonPin != kSi3050PinRgdt && kDevRingButtonPin != kSi3050PinSpiCs,
              "DEV ring simulator button GPIO must never collide with a real Si3050 pin");
static_assert(kDevRingButtonPin != kSi3050ReservedPinBoot && kDevRingButtonPin != kSi3050ReservedPinUsbDMinus &&
                  kDevRingButtonPin != kSi3050ReservedPinUsbDPlus,
              "DEV ring simulator button GPIO must never collide with BOOT/USB pins");

} // namespace interbridge
