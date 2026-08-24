#pragma once

#include <cstddef>
#include <cstdint>

namespace interbridge {

// Si3050 Rev A GPIO pin map for the ESP32-C3 target. Single source of
// truth for pin *numbers* - see
// https://github.com/hjca14/interhardware/blob/main/docs/decisions/INTERBRIDGE_GPIO_MAP.md
// for the full electrical rationale (pull directions, active levels,
// external component values). This header only records pin numbers and
// enforces at compile time that they never collide with reserved
// ESP32-C3 pins; it does not configure any pin by itself - see
// si3050_bus.h / si3050_reset.h / ring_detector.h for that.
constexpr uint8_t kSi3050PinPclk = 0;    // PCM clock, ESP -> Si3050
constexpr uint8_t kSi3050PinFsync = 1;   // PCM frame sync, ESP -> Si3050
constexpr uint8_t kSi3050PinSpiMiso = 2; // SDO, Si3050 -> ESP; external 10k pull-up to 3.3V
constexpr uint8_t kSi3050PinPcmDtx = 3;  // DTX, Si3050 -> ESP
constexpr uint8_t kSi3050PinPcmDrx = 4;  // DRX, ESP -> Si3050
constexpr uint8_t kSi3050PinReset = 5;   // /RESET, ESP -> Si3050; active low, external pull-down
constexpr uint8_t kSi3050PinSpiSclk = 6; // SCLK, ESP -> Si3050
constexpr uint8_t kSi3050PinSpiMosi = 7; // SDI, ESP -> Si3050
constexpr uint8_t kSi3050PinRgdt = 8;    // /RGDT, Si3050 -> ESP; open-drain, active low, external 4.7k pull-up
constexpr uint8_t kSi3050PinSpiCs = 10;  // CS, ESP -> Si3050; active low

// AOUT/INT (Si3050 pin 7) is NOT wired to the ESP in this Rev A - it stays
// on a bare bench pad. Do not add a pin constant for it and do not assume
// any use for it here.

// Reserved by the ESP32-C3 module/board itself - the Si3050 driver must
// never claim any of these. GPIO9 (BOOT) in particular must stay reserved
// for ROM download-mode recovery, never repurposed for Si3050 use.
constexpr uint8_t kSi3050ReservedPinBoot = 9;
constexpr uint8_t kSi3050ReservedPinUsbDMinus = 18;
constexpr uint8_t kSi3050ReservedPinUsbDPlus = 19;
constexpr uint8_t kSi3050ReservedPinButton = 20;
constexpr uint8_t kSi3050ReservedPinStatusLed = 21;

namespace si3050_detail {

constexpr uint8_t kSi3050Pins[] = {
    kSi3050PinPclk,   kSi3050PinFsync,   kSi3050PinSpiMiso, kSi3050PinPcmDtx, kSi3050PinPcmDrx,
    kSi3050PinReset,  kSi3050PinSpiSclk, kSi3050PinSpiMosi, kSi3050PinRgdt,   kSi3050PinSpiCs,
};

constexpr uint8_t kSi3050ReservedPins[] = {
    kSi3050ReservedPinBoot,
    kSi3050ReservedPinUsbDMinus,
    kSi3050ReservedPinUsbDPlus,
    kSi3050ReservedPinButton,
    kSi3050ReservedPinStatusLed,
};

constexpr std::size_t kSi3050PinCount = sizeof(kSi3050Pins) / sizeof(kSi3050Pins[0]);
constexpr std::size_t kSi3050ReservedPinCount = sizeof(kSi3050ReservedPins) / sizeof(kSi3050ReservedPins[0]);

constexpr bool contains(const uint8_t* values, std::size_t count, uint8_t target) {
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] == target) return true;
    }
    return false;
}

// Checked at compile time below via static_assert - kept as a callable
// constexpr function (rather than only a static_assert body) so native
// tests can also exercise it as an explicit, discoverable test case.
constexpr bool noReservedPinOverlap() {
    for (std::size_t i = 0; i < kSi3050PinCount; ++i) {
        if (contains(kSi3050ReservedPins, kSi3050ReservedPinCount, kSi3050Pins[i])) return false;
    }
    return true;
}

} // namespace si3050_detail

static_assert(si3050_detail::noReservedPinOverlap(),
              "Si3050 pin map collides with a reserved ESP32-C3 GPIO (USB/BOOT/button/LED)");
static_assert(kSi3050ReservedPinBoot == 9,
              "GPIO9 must remain reserved for BOOT/download-mode recovery - do not repurpose for Si3050");
static_assert(kSi3050PinRgdt == 8, "GPIO8 is committed to /RGDT per the Rev A pin contract");

} // namespace interbridge
