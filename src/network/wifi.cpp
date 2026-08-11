#include "wifi.h"

#include <Arduino.h>
#include <WiFi.h>

namespace interbridge {

WifiManager::WifiManager() : started_(false), lastConnected_(false) {}

void WifiManager::begin(const WifiCredentials& credentials) {
    started_ = true;
    lastConnected_ = false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(credentials.ssid, credentials.password);
}

std::optional<Event> WifiManager::update() {
    if (!started_) {
        return std::nullopt;
    }

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected == lastConnected_) {
        return std::nullopt;
    }

    lastConnected_ = connected;
    return Event{connected ? EventType::WifiConnected : EventType::WifiDisconnected};
}

bool WifiManager::isConnected() const {
    return lastConnected_;
}

} // namespace interbridge
