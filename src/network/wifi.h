#pragma once

#include <optional>

#include "../core/events.h"

namespace interbridge {

// Wi-Fi is the confirmed transport between the ESP32 and the InterBridge
// backend/app, but HOW credentials are provisioned (hardcoded for now,
// NVS, BLE/captive-portal provisioning, etc.) has not been decided yet.
// See CONTEXT.md > Open Questions.
struct WifiCredentials {
    const char* ssid;
    const char* password;
};

// Thin wrapper around the ESP32 Wi-Fi station API. Kept separate from
// network/protocol.h: this module only owns the Wi-Fi link, never the
// InterBridge application protocol running on top of it.
class WifiManager {
public:
    WifiManager();

    void begin(const WifiCredentials& credentials);

    // Polls the connection status and returns an event if it changed
    // since begin() was called. Returns std::nullopt if nothing changed,
    // and always returns std::nullopt before begin() has been called.
    std::optional<Event> update();

    bool isConnected() const;

private:
    bool started_;
    bool lastConnected_;
};

} // namespace interbridge
