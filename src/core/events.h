#pragma once

namespace interbridge {

// Strongly-typed events flowing through the system. This is the initial
// set required to represent the basic InterBridge call flow; it is
// expected to grow (e.g. door feedback, audio session events, OTA events)
// as hardware and protocol decisions are made. See CONTEXT.md.
enum class EventType {
    RingDetected,
    OffHook,
    OnHook,
    DoorOpenRequested,
    CallStarted,
    CallEnded,
    WifiConnected,
    WifiDisconnected,
};

struct Event {
    EventType type;
};

// Human-readable name used for logging. Kept separate from EventType so
// the enum stays a plain, lightweight type.
const char* toString(EventType type);

} // namespace interbridge
