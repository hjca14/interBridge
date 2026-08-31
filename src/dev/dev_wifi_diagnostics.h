#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace interbridge {

// Phase 3B.8 DEV Wi-Fi diagnostics. Every function here is pure and
// hardware-independent - none of them ever receive or handle a raw
// SSID/password value, only byte-length/empty/placeholder facts about
// one, or a scan result's public (never secret) SSID for matching
// purposes only. This module is shared, unmodified, between
// mqtt_smoke_main.cpp and dev_ring_simulator_main.cpp so neither
// duplicates this logic - only the small Arduino-only glue (reading
// WiFi.SSID()/RSSI()/channel()/encryptionType(), calling
// WiFi.scanNetworks(), and the Serial.print calls themselves) lives in
// each main, same pattern as DevMqttSmokeState. See
// docs/dev-ring-simulator.md for the real-hardware investigation this
// was added for.

// ---- Credential config (SSID/password) diagnostics ----

struct CredentialFieldDiagnostics {
    size_t lengthBytes = 0;
    bool empty = false;
    bool matchesPlaceholder = false;
};

// Reports only length/empty/placeholder-match facts about one DEV secret
// field - never the value itself. `placeholder` is the exact string
// include/interbridge_dev_secrets.example.h uses for this field; the
// caller passes it in so this module never needs to know that contract.
CredentialFieldDiagnostics diagnoseCredentialField(const std::string& value, const std::string& placeholder);

struct CredentialConfigSummary {
    bool valid = false;             // !empty && !placeholder for both fields
    size_t ssidLengthBytes = 0;
    size_t passwordLengthBytes = 0;
    bool placeholderDetected = false; // either field still matches its example-header placeholder
};

CredentialConfigSummary summarizeCredentialConfig(const CredentialFieldDiagnostics& ssid,
                                                  const CredentialFieldDiagnostics& password);

// "config=valid|invalid ssid_bytes=N password_bytes=N placeholder=true|false" -
// never includes the raw ssid/password value, by construction (this
// function is never given one).
std::string formatCredentialConfigLine(const CredentialConfigSummary& summary);

// ---- Wi-Fi scan diagnostics ----

struct WifiScanNetwork {
    std::string ssid; // only ever compared, never logged directly - see summarizeWifiScan()
    int32_t rssi = 0;
    int32_t channel = 0;
    std::string authType; // already-sanitized label, e.g. "wpa2_psk" - see the Arduino-only mapping in each main
};

struct WifiScanSummary {
    size_t networksFound = 0;
    bool configuredSsidFound = false;
    int32_t rssi = 0;
    int32_t channel = 0;
    std::string authType = "none";
};

// Summarizes one Wi-Fi scan relative to the configured SSID. Only the
// total network count and the configured SSID's own RSSI/channel/auth
// type (if present) are retained - every OTHER network's SSID is
// discarded immediately after the comparison and never appears in the
// returned struct or anywhere downstream.
WifiScanSummary summarizeWifiScan(const std::vector<WifiScanNetwork>& networks, const std::string& configuredSsid);

// "networks_found=N configured_ssid_found=true|false rssi=N channel=N
// auth=... scan_age_ms=N" - scanAgeMs is the caller's own
// now-minus-last-scan-time computation (see
// DevMqttSmokeState::millisUntil(), reused for this exact "elapsed since"
// shape by calling it as millisUntil(now, lastScanAtMs)). Never includes
// any network's SSID/name, by construction (WifiScanSummary itself never
// carries one).
std::string formatWifiScanLine(const WifiScanSummary& summary, uint32_t scanAgeMs);

} // namespace interbridge
