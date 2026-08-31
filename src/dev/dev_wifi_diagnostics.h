#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
// field - never the value itself. Takes std::string_view (a non-owning
// view), not std::string, specifically so this never allocates or copies
// the secret onto the heap - this function is called on every heartbeat
// tick while Wi-Fi is down, and a real std::string parameter would make
// that a fresh heap copy of the Wi-Fi password every ~15s for as long as
// the device stays offline. `placeholder` is the exact string
// include/interbridge_dev_secrets.example.h uses for this field; the
// caller passes it in so this module never needs to know that contract.
CredentialFieldDiagnostics diagnoseCredentialField(std::string_view value, std::string_view placeholder);

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
// function is never given one). Building this small, bounded line is the
// one place this module allocates a std::string - the returned text is
// pure metadata (lengths/booleans), never the secret itself.
std::string formatCredentialConfigLine(const CredentialConfigSummary& summary);

// ---- Wi-Fi scan diagnostics ----

struct WifiScanNetwork {
    std::string ssid; // only ever compared, never logged directly - see summarizeWifiScan()
    int32_t rssi = 0;
    int32_t channel = 0;
    std::string authType; // already-sanitized label, e.g. "wpa2_psk" - see the Arduino-only mapping in each main
};

// Success: WiFi.scanNetworks() itself completed (even if it found zero
// networks - see summarizeWifiScan()). Failed: the scan call itself
// returned an error (e.g. WIFI_SCAN_FAILED) - configuredSsidFound/rssi/
// channel/authType are not meaningful in this case and callers must not
// interpret them; only errorCode is.
enum class WifiScanStatus { Success, Failed };

struct WifiScanSummary {
    WifiScanStatus status = WifiScanStatus::Success;
    size_t networksFound = 0;
    bool configuredSsidFound = false;
    int32_t rssi = 0;
    int32_t channel = 0;
    std::string authType = "none";
    int32_t errorCode = 0; // only meaningful when status == Failed - the raw WiFi.scanNetworks() return value
};

// Summarizes one successfully completed Wi-Fi scan relative to the
// configured SSID (never call this for a failed scan - see
// makeFailedWifiScanSummary()). Only the total network count and the
// configured SSID's own RSSI/channel/auth type (if present) are retained
// - every OTHER network's SSID is discarded immediately after the
// comparison and never appears in the returned struct or anywhere
// downstream. `configuredSsid` is a view, not an owning copy, for the
// same no-heap-copy-of-the-secret reason as diagnoseCredentialField().
// An empty `networks` list legitimately means "scan succeeded, found
// zero networks" - distinct from a failed scan, which callers must
// represent with makeFailedWifiScanSummary() instead, never with an
// empty networks list here.
WifiScanSummary summarizeWifiScan(const std::vector<WifiScanNetwork>& networks, std::string_view configuredSsid);

// Builds a WifiScanSummary representing a scan call that itself failed
// (e.g. WiFi.scanNetworks() returned a negative WIFI_SCAN_* code) -
// distinguishable from summarizeWifiScan() with an empty list, which
// represents a scan that succeeded but found zero networks.
WifiScanSummary makeFailedWifiScanSummary(int32_t errorCode);

// Success: "scan_status=success networks_found=N configured_ssid_found=
// true|false rssi=N channel=N auth=... scan_age_ms=N". Failed:
// "scan_status=failed error=N scan_age_ms=N" - configured_ssid_found/
// rssi/channel/auth are omitted entirely rather than printed as
// misleading zeros/false. scanAgeMs is the caller's own
// now-minus-last-scan-time computation (see
// DevMqttSmokeState::millisUntil(), reused for this exact "elapsed
// since" shape by calling it as millisUntil(now, lastScanAtMs)). Never
// includes any network's SSID/name, by construction (WifiScanSummary
// itself never carries one).
std::string formatWifiScanLine(const WifiScanSummary& summary, uint32_t scanAgeMs);

} // namespace interbridge
