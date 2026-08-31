#include "dev_wifi_diagnostics.h"

#include <cstdio>

namespace interbridge {

CredentialFieldDiagnostics diagnoseCredentialField(std::string_view value, std::string_view placeholder) {
    CredentialFieldDiagnostics result;
    result.lengthBytes = value.size();
    result.empty = value.empty();
    result.matchesPlaceholder = (value == placeholder);
    return result;
}

CredentialConfigSummary summarizeCredentialConfig(const CredentialFieldDiagnostics& ssid,
                                                  const CredentialFieldDiagnostics& password) {
    CredentialConfigSummary summary;
    summary.ssidLengthBytes = ssid.lengthBytes;
    summary.passwordLengthBytes = password.lengthBytes;
    summary.placeholderDetected = ssid.matchesPlaceholder || password.matchesPlaceholder;
    summary.valid = !ssid.empty && !password.empty && !summary.placeholderDetected;
    return summary;
}

std::string formatCredentialConfigLine(const CredentialConfigSummary& summary) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "config=%s ssid_bytes=%u password_bytes=%u placeholder=%s",
                  summary.valid ? "valid" : "invalid", static_cast<unsigned>(summary.ssidLengthBytes),
                  static_cast<unsigned>(summary.passwordLengthBytes),
                  summary.placeholderDetected ? "true" : "false");
    return std::string(buf);
}

WifiScanSummary summarizeWifiScan(const std::vector<WifiScanNetwork>& networks, std::string_view configuredSsid) {
    WifiScanSummary summary;
    summary.status = WifiScanStatus::Success;
    summary.networksFound = networks.size();
    for (const auto& network : networks) {
        if (network.ssid == configuredSsid) {
            summary.configuredSsidFound = true;
            summary.rssi = network.rssi;
            summary.channel = network.channel;
            summary.authType = network.authType;
            break;
        }
    }
    return summary;
}

WifiScanSummary makeFailedWifiScanSummary(int32_t errorCode) {
    WifiScanSummary summary;
    summary.status = WifiScanStatus::Failed;
    summary.errorCode = errorCode;
    return summary;
}

std::string formatWifiScanLine(const WifiScanSummary& summary, uint32_t scanAgeMs) {
    char buf[224];
    if (summary.status == WifiScanStatus::Failed) {
        std::snprintf(buf, sizeof(buf), "scan_status=failed error=%d scan_age_ms=%lu",
                      static_cast<int>(summary.errorCode), static_cast<unsigned long>(scanAgeMs));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "scan_status=success networks_found=%u configured_ssid_found=%s rssi=%d channel=%d auth=%s "
                      "scan_age_ms=%lu",
                      static_cast<unsigned>(summary.networksFound), summary.configuredSsidFound ? "true" : "false",
                      static_cast<int>(summary.rssi), static_cast<int>(summary.channel), summary.authType.c_str(),
                      static_cast<unsigned long>(scanAgeMs));
    }
    return std::string(buf);
}

} // namespace interbridge
