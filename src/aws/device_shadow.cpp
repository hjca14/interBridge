#include "device_shadow.h"

#define ARDUINOJSON_ENABLE_STD_STRING 1
#include <ArduinoJson.h>

namespace interbridge {

std::string DeviceShadow::buildReportedUpdate(const ShadowReportedState& state) {
    JsonDocument doc;
    JsonObject reported = doc["state"]["reported"].to<JsonObject>();
    reported["firmware_version"] = state.firmwareVersion;
    reported["hardware_version"] = state.hardwareVersion;
    reported["intercom_state"] = state.intercomState;
    reported["wifi_rssi"] = state.wifiRssi;
    reported["uptime_ms"] = state.uptimeMs;
    reported["provisioned"] = state.provisioned;
    reported["health_interval_s"] = state.healthIntervalSeconds;

    std::string out;
    serializeJson(doc, out);
    return out;
}

ShadowDelta DeviceShadow::parseDelta(const std::string& deltaJson) {
    ShadowDelta delta;

    JsonDocument doc;
    if (deserializeJson(doc, deltaJson)) {
        return delta; // malformed - fail safe, nothing changed
    }

    JsonVariant root = doc.as<JsonVariant>();
    JsonVariant healthInterval = root["state"]["health_interval_s"];
    if (healthInterval.isNull()) {
        healthInterval = root["health_interval_s"];
    }

    if (healthInterval.is<uint32_t>()) {
        delta.healthIntervalChanged = true;
        delta.healthIntervalSeconds = healthInterval.as<uint32_t>();
    }

    return delta;
}

} // namespace interbridge
