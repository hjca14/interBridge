#include "events.h"

namespace interbridge {

const char* toString(EventType type) {
    switch (type) {
        case EventType::RingDetected: return "RING_DETECTED";
        case EventType::OffHook: return "OFF_HOOK";
        case EventType::OnHook: return "ON_HOOK";
        case EventType::DoorOpenRequested: return "DOOR_OPEN_REQUESTED";
        case EventType::CallStarted: return "CALL_STARTED";
        case EventType::CallEnded: return "CALL_ENDED";
        case EventType::WifiConnected: return "WIFI_CONNECTED";
        case EventType::WifiDisconnected: return "WIFI_DISCONNECTED";
    }
    return "UNKNOWN_EVENT";
}

} // namespace interbridge
