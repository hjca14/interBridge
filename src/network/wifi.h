#pragma once

#include <optional>
#include <string>

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

// Abstraction over the Wi-Fi station link. Business logic that only
// needs "am I online" / "connect with these credentials" (see
// provisioning/provisioning_manager.h) depends on this interface, not on
// the concrete ESP32 WifiManager, so it stays testable without a real
// Wi-Fi radio - see FakeWifiConnection.
class IWifiConnection {
public:
    virtual ~IWifiConnection() = default;

    virtual void begin(const WifiCredentials& credentials) = 0;

    // Polls the connection status and returns an event if it changed
    // since begin() was called. Returns std::nullopt if nothing changed,
    // and always returns std::nullopt before begin() has been called.
    virtual std::optional<Event> update() = 0;

    virtual bool isConnected() const = 0;
};

// Thin wrapper around the ESP32 Wi-Fi station API. Kept separate from
// network/protocol.h: this module only owns the Wi-Fi link, never the
// InterBridge application protocol running on top of it.
class WifiManager : public IWifiConnection {
public:
    WifiManager();

    void begin(const WifiCredentials& credentials) override;
    std::optional<Event> update() override;
    bool isConnected() const override;

private:
    bool started_;
    bool lastConnected_;
};

// Test double: begin() just records the credentials; connection state is
// driven explicitly via setConnected(), letting tests simulate Wi-Fi
// connecting/dropping without any real radio.
class FakeWifiConnection : public IWifiConnection {
public:
    FakeWifiConnection();

    void begin(const WifiCredentials& credentials) override;
    std::optional<Event> update() override;
    bool isConnected() const override;

    void setConnected(bool connected);
    bool beginCalled() const;
    const std::string& lastSsid() const;

private:
    bool started_;
    bool connected_;
    bool lastConnected_;
    std::string lastSsid_;
};

} // namespace interbridge
