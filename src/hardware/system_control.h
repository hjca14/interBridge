#pragma once

namespace interbridge {

// System-level actions that don't fit the intercom/audio/network
// abstractions. Kept as a narrow interface so command_handler (RESTART)
// never calls ESP.restart() directly and stays testable.
class ISystemControl {
public:
    virtual ~ISystemControl() = default;

    // Performs a software restart. Does not return on real hardware.
    virtual void restart() = 0;
};

// Real ESP32 implementation, backed by the Arduino core's ESP.restart().
class Esp32SystemControl : public ISystemControl {
public:
    void restart() override;
};

// Test double: records whether/how many times restart() was requested
// instead of actually resetting the process.
class FakeSystemControl : public ISystemControl {
public:
    FakeSystemControl();

    void restart() override;

    int restartCount() const;

private:
    int restartCount_;
};

} // namespace interbridge
