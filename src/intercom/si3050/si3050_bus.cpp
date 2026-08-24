#include "si3050_bus.h"

#include "si3050_pins.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

void Esp32Si3050Bus::begin() {
#ifdef ARDUINO
    pinMode(kSi3050PinSpiCs, OUTPUT);
    digitalWrite(kSi3050PinSpiCs, HIGH); // deselected (active low)
    pinMode(kSi3050PinSpiSclk, OUTPUT);
    pinMode(kSi3050PinSpiMosi, OUTPUT);
    pinMode(kSi3050PinSpiMiso, INPUT); // external 10k pull-up already present - no internal pull needed
#endif
}

void Esp32Si3050Bus::setChipSelect(bool selected) {
#ifdef ARDUINO
    digitalWrite(kSi3050PinSpiCs, selected ? LOW : HIGH);
#else
    (void)selected;
#endif
}

void Esp32Si3050Bus::holdClockIdleHigh() {
#ifdef ARDUINO
    digitalWrite(kSi3050PinSpiSclk, HIGH);
#endif
}

uint8_t Esp32Si3050Bus::transfer(uint8_t out) {
    (void)out;
    // TODO: not implemented. The SPI transaction electrical mode (clock
    // polarity/phase) for the Si3050 is not confirmed against real
    // hardware in this repository - wire this to the ESP32 SPI peripheral
    // (or bit-bang it) once the datasheet/bench confirms it. Never called
    // by Si3050Controller before initialize() completes - see
    // Si3050Controller::transferRaw().
    return 0;
}

void FakeSi3050Bus::begin() {
    beginCalled = true;
    log("bus.begin");
}

void FakeSi3050Bus::setChipSelect(bool selected) {
    chipSelected = selected;
    log(selected ? "bus.cs.select" : "bus.cs.deselect");
}

void FakeSi3050Bus::holdClockIdleHigh() {
    sclkHeldHigh = true;
    log("bus.sclkIdleHigh");
}

uint8_t FakeSi3050Bus::transfer(uint8_t out) {
    ++transferCallCount;
    lastTransferOut = out;
    log("bus.transfer");
    return nextTransferReturn;
}

void FakeSi3050Bus::log(const char* tag) {
    if (log_) log_->emplace_back(tag);
}

} // namespace interbridge
