#pragma once

namespace interbridge {

// Abstraction over the physical inputs/outputs InterBridge depends on.
// High-level code (intercom, state machine, coordinators) must depend on
// this interface only - never on digitalRead/digitalWrite, GPIO numbers,
// or voltage levels directly. This keeps that code testable without real
// hardware (see test/test_line_detector for an example using a mock).
//
// IMPORTANT: the real electrical interface is NOT defined yet:
//   - which GPIOs are used;
//   - what voltage levels/logic represent "line active" vs "idle";
//   - how off-hook is actually detected on the physical intercom line;
//   - how the door release output is actually driven (relay? transistor?
//     pulse duration? active-high or active-low?).
// See CONTEXT.md > Open Questions. Esp32GpioHardware below is a stub
// until those decisions are made.
class IHardwareIO {
public:
    virtual ~IHardwareIO() = default;

    // Returns the current raw state of the intercom line. What "true"
    // means electrically is not yet defined.
    virtual bool readLineState() = 0;

    // Enables/disables the door release output. Actuation semantics
    // (momentary pulse vs. sustained, active-high vs. active-low) are not
    // yet defined. Returns whether the output was genuinely driven -
    // implementations must return false rather than pretending success
    // when actuation isn't actually implemented (see Esp32GpioHardware).
    virtual bool setDoorOutput(bool enabled) = 0;
};

// Concrete ESP32-C3 implementation. Currently a stub: every method has a
// placeholder body because the GPIO mapping and electrical behavior have
// not been decided yet. Do not treat this as functional hardware support -
// setDoorOutput() always returns false because nothing is actually
// actuated.
class Esp32GpioHardware : public IHardwareIO {
public:
    Esp32GpioHardware();

    bool readLineState() override;
    bool setDoorOutput(bool enabled) override;
};

} // namespace interbridge
