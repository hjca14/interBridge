#pragma once

namespace interbridge {

// Semantic feedback the firmware wants to show the user during
// onboarding - see docs/communication-protocol.md > LED/Status
// Indication. This is intentionally an enum of *meanings*, not blink
// patterns/colors/timings: the LED GPIO, electrical behavior, and final
// blink/color design are not defined yet (hardware not finalized) - see
// CONTEXT.md > Open Questions. A concrete IStatusIndicator implementation
// owns translating a semantic indication into actual hardware behavior
// once that's decided.
enum class ProvisioningIndication {
    ProvisioningAvailable, // advertising, waiting for the app to connect
    AppConnected,           // a BLE session is active (identification feedback)
    ProvisioningSuccess,
    ProvisioningFailure,
    FactoryResetWarning,     // shown before/during a destructive factory reset
};

class IStatusIndicator {
public:
    virtual ~IStatusIndicator() = default;

    virtual void show(ProvisioningIndication indication) = 0;

    // Returns to whatever the indicator shows during normal operation
    // (e.g. off, or a steady "ready" indication - not defined yet).
    virtual void clear() = 0;
};

// Real ESP32 implementation. STUB: no LED GPIO or blink/color design has
// been chosen yet. See CONTEXT.md > Open Questions.
class Esp32StatusIndicator : public IStatusIndicator {
public:
    void show(ProvisioningIndication indication) override;
    void clear() override;
};

// Test double: records the most recent indication (or "cleared") so
// tests can assert on it without any real hardware.
class FakeStatusIndicator : public IStatusIndicator {
public:
    FakeStatusIndicator();

    void show(ProvisioningIndication indication) override;
    void clear() override;

    bool hasIndication() const;
    ProvisioningIndication lastIndication() const;
    int showCount() const;

private:
    bool hasIndication_;
    ProvisioningIndication lastIndication_;
    int showCount_;
};

} // namespace interbridge
