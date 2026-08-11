// FakeWifiConnection lives in its own translation unit (rather than
// wifi.cpp) because wifi.cpp is excluded from the native test build (it
// depends on Arduino WiFi.h), but the fake has no such dependency and
// must be available natively - see platformio.ini > [env:native].
#include "wifi.h"

namespace interbridge {

FakeWifiConnection::FakeWifiConnection() : started_(false), connected_(false), lastConnected_(false) {}

void FakeWifiConnection::begin(const WifiCredentials& credentials) {
    started_ = true;
    lastConnected_ = false;
    lastSsid_ = credentials.ssid ? credentials.ssid : "";
}

std::optional<Event> FakeWifiConnection::update() {
    if (!started_) {
        return std::nullopt;
    }
    if (connected_ == lastConnected_) {
        return std::nullopt;
    }
    lastConnected_ = connected_;
    return Event{connected_ ? EventType::WifiConnected : EventType::WifiDisconnected};
}

bool FakeWifiConnection::isConnected() const {
    return lastConnected_;
}

void FakeWifiConnection::setConnected(bool connected) {
    connected_ = connected;
}

bool FakeWifiConnection::beginCalled() const {
    return started_;
}

const std::string& FakeWifiConnection::lastSsid() const {
    return lastSsid_;
}

} // namespace interbridge
