#include <unity.h>

#include <string_view>

#include "../../src/dev/dev_wifi_diagnostics.h"

using namespace interbridge;

namespace {
constexpr const char* kSsidPlaceholder = "REPLACE_WITH_WIFI_SSID";
constexpr const char* kPasswordPlaceholder = "REPLACE_WITH_WIFI_PASSWORD";
} // namespace

void setUp() {}
void tearDown() {}

void test_placeholder_value_is_detected() {
    auto diag = diagnoseCredentialField(kSsidPlaceholder, kSsidPlaceholder);
    TEST_ASSERT_TRUE(diag.matchesPlaceholder);
    TEST_ASSERT_FALSE(diag.empty);
    TEST_ASSERT_EQUAL(std::string(kSsidPlaceholder).size(), diag.lengthBytes);
}

void test_non_placeholder_value_is_not_flagged() {
    auto diag = diagnoseCredentialField("MyRealNetwork", kSsidPlaceholder);
    TEST_ASSERT_FALSE(diag.matchesPlaceholder);
}

void test_field_lengths_are_correct() {
    auto empty = diagnoseCredentialField("", "x");
    TEST_ASSERT_EQUAL(0, static_cast<int>(empty.lengthBytes));
    TEST_ASSERT_TRUE(empty.empty);

    auto nonEmpty = diagnoseCredentialField("hello", "x");
    TEST_ASSERT_EQUAL(5, static_cast<int>(nonEmpty.lengthBytes));
    TEST_ASSERT_FALSE(nonEmpty.empty);
}

void test_config_summary_is_valid_only_when_both_fields_are_real_and_non_empty() {
    auto goodSsid = diagnoseCredentialField("MyRealNetwork", kSsidPlaceholder);
    auto goodPassword = diagnoseCredentialField("a-real-password", kPasswordPlaceholder);
    CredentialConfigSummary valid = summarizeCredentialConfig(goodSsid, goodPassword);
    TEST_ASSERT_TRUE(valid.valid);
    TEST_ASSERT_FALSE(valid.placeholderDetected);

    auto placeholderSsid = diagnoseCredentialField(kSsidPlaceholder, kSsidPlaceholder);
    CredentialConfigSummary withPlaceholder = summarizeCredentialConfig(placeholderSsid, goodPassword);
    TEST_ASSERT_FALSE(withPlaceholder.valid);
    TEST_ASSERT_TRUE(withPlaceholder.placeholderDetected);

    auto emptyPassword = diagnoseCredentialField("", kPasswordPlaceholder);
    CredentialConfigSummary withEmpty = summarizeCredentialConfig(goodSsid, emptyPassword);
    TEST_ASSERT_FALSE(withEmpty.valid);
    TEST_ASSERT_FALSE(withEmpty.placeholderDetected); // empty, but not the placeholder string itself
}

// The formatted line must never include the raw ssid/password value -
// verified with distinctive marker strings run through the entire
// diagnostic pipeline, not just asserted by inspection.
void test_credential_config_line_never_includes_secret_values() {
    const std::string secretSsid = "MySecretNetworkName";
    const std::string secretPassword = "TopSecretPassword123";
    auto ssidDiag = diagnoseCredentialField(secretSsid, kSsidPlaceholder);
    auto passwordDiag = diagnoseCredentialField(secretPassword, kPasswordPlaceholder);
    CredentialConfigSummary summary = summarizeCredentialConfig(ssidDiag, passwordDiag);
    std::string line = formatCredentialConfigLine(summary);

    TEST_ASSERT_TRUE(line.find(secretSsid) == std::string::npos);
    TEST_ASSERT_TRUE(line.find(secretPassword) == std::string::npos);
    TEST_ASSERT_TRUE(line.find("config=valid") != std::string::npos);
    TEST_ASSERT_TRUE(line.find("ssid_bytes=" + std::to_string(secretSsid.size())) != std::string::npos);
    TEST_ASSERT_TRUE(line.find("password_bytes=" + std::to_string(secretPassword.size())) != std::string::npos);
    TEST_ASSERT_TRUE(line.find("placeholder=false") != std::string::npos);
}

void test_scan_finds_configured_ssid() {
    std::vector<WifiScanNetwork> networks = {
        {"NeighborNetwork", -80, 11, "open"},
        {"HomeNetwork", -45, 6, "wpa2_psk"},
        {"AnotherNetwork", -70, 1, "wpa_wpa2_psk"},
    };
    WifiScanSummary summary = summarizeWifiScan(networks, "HomeNetwork");
    TEST_ASSERT_EQUAL(3, static_cast<int>(summary.networksFound));
    TEST_ASSERT_TRUE(summary.configuredSsidFound);
    TEST_ASSERT_EQUAL(-45, static_cast<int>(summary.rssi));
    TEST_ASSERT_EQUAL(6, static_cast<int>(summary.channel));
    TEST_ASSERT_EQUAL_STRING("wpa2_psk", summary.authType.c_str());
}

void test_scan_does_not_find_configured_ssid() {
    std::vector<WifiScanNetwork> networks = {
        {"NeighborNetwork", -80, 11, "open"},
        {"AnotherNetwork", -70, 1, "wpa_wpa2_psk"},
    };
    WifiScanSummary summary = summarizeWifiScan(networks, "HomeNetwork");
    TEST_ASSERT_EQUAL(2, static_cast<int>(summary.networksFound));
    TEST_ASSERT_FALSE(summary.configuredSsidFound);
    TEST_ASSERT_EQUAL_STRING("none", summary.authType.c_str());
}

void test_empty_scan_reports_zero_networks_and_not_found() {
    WifiScanSummary summary = summarizeWifiScan({}, "HomeNetwork");
    TEST_ASSERT_TRUE(summary.status == WifiScanStatus::Success);
    TEST_ASSERT_EQUAL(0, static_cast<int>(summary.networksFound));
    TEST_ASSERT_FALSE(summary.configuredSsidFound);
}

// A scan that succeeded but genuinely found zero networks must be
// distinguishable from a scan call that itself failed - both would
// otherwise look identical ("networks_found=0
// configured_ssid_found=false") to anything reading only those two
// fields, which is exactly the ambiguity this status field closes.
void test_valid_scan_with_zero_networks_differs_from_failed_scan() {
    WifiScanSummary validZero = summarizeWifiScan({}, "AnySsid");
    WifiScanSummary failed = makeFailedWifiScanSummary(-2);

    TEST_ASSERT_TRUE(validZero.status == WifiScanStatus::Success);
    TEST_ASSERT_TRUE(failed.status == WifiScanStatus::Failed);

    std::string validLine = formatWifiScanLine(validZero, 100);
    std::string failedLine = formatWifiScanLine(failed, 100);
    TEST_ASSERT_TRUE(validLine.find("scan_status=success") != std::string::npos);
    TEST_ASSERT_TRUE(validLine.find("networks_found=0") != std::string::npos);
    TEST_ASSERT_TRUE(failedLine.find("scan_status=failed") != std::string::npos);
    TEST_ASSERT_TRUE(failedLine.find("error=-2") != std::string::npos);
    // A failed scan never prints configured_ssid_found/networks_found at
    // all - they would be meaningless (the scan itself never completed),
    // not just false/zero.
    TEST_ASSERT_TRUE(failedLine.find("configured_ssid_found") == std::string::npos);
    TEST_ASSERT_TRUE(failedLine.find("networks_found") == std::string::npos);
}

// The failed-scan status/error must keep showing up correctly every time
// the same stored summary is reformatted later (e.g. once at scan time,
// then again from every subsequent heartbeat/pre-WiFi.begin() log line
// while Wi-Fi stays down) - not just on the first read right after the
// scan failed.
void test_failed_scan_status_persists_across_repeated_formatting() {
    WifiScanSummary failed = makeFailedWifiScanSummary(-2);
    std::string firstRead = formatWifiScanLine(failed, 0);
    std::string muchLaterRead = formatWifiScanLine(failed, 999999);
    TEST_ASSERT_TRUE(firstRead.find("scan_status=failed") != std::string::npos);
    TEST_ASSERT_TRUE(firstRead.find("error=-2") != std::string::npos);
    TEST_ASSERT_TRUE(muchLaterRead.find("scan_status=failed") != std::string::npos);
    TEST_ASSERT_TRUE(muchLaterRead.find("error=-2") != std::string::npos);
    TEST_ASSERT_TRUE(muchLaterRead.find("scan_age_ms=999999") != std::string::npos);
}

// diagnoseCredentialField() must work directly against a non-owning view
// - constructed here from a raw const char*, never from an owning
// std::string - proving (not just documenting) that no std::string copy
// of the secret is required to call it. If this function's signature
// ever regressed back to `const std::string&`, this line would fail to
// compile (std::string_view has no implicit conversion to std::string),
// making that regression a build failure here rather than a silent
// runtime behavior change.
void test_diagnose_credential_field_works_from_a_non_owning_view_without_copying() {
    const char* rawSsid = "RawCStringSsidValue";
    std::string_view view(rawSsid);
    CredentialFieldDiagnostics diag = diagnoseCredentialField(view, kSsidPlaceholder);
    TEST_ASSERT_EQUAL(static_cast<int>(view.size()), static_cast<int>(diag.lengthBytes));
    TEST_ASSERT_FALSE(diag.matchesPlaceholder);
    TEST_ASSERT_FALSE(diag.empty);
}

// The formatted scan line must never include any network's SSID/name -
// not the configured one, and not any other network scanned alongside
// it - verified with distinctive marker strings, whether the configured
// SSID was found or not.
void test_scan_line_never_includes_network_names() {
    std::vector<WifiScanNetwork> networks = {
        {"NeighborNetworkSecretName", -80, 11, "open"},
        {"HomeNetworkSecretName", -45, 6, "wpa2_psk"},
    };

    WifiScanSummary found = summarizeWifiScan(networks, "HomeNetworkSecretName");
    std::string lineFound = formatWifiScanLine(found, 1234);
    TEST_ASSERT_TRUE(lineFound.find("HomeNetworkSecretName") == std::string::npos);
    TEST_ASSERT_TRUE(lineFound.find("NeighborNetworkSecretName") == std::string::npos);
    TEST_ASSERT_TRUE(lineFound.find("scan_status=success") != std::string::npos);
    TEST_ASSERT_TRUE(lineFound.find("configured_ssid_found=true") != std::string::npos);
    TEST_ASSERT_TRUE(lineFound.find("scan_age_ms=1234") != std::string::npos);

    WifiScanSummary notFound = summarizeWifiScan(networks, "SomeOtherSecretNetworkName");
    std::string lineNotFound = formatWifiScanLine(notFound, 5678);
    TEST_ASSERT_TRUE(lineNotFound.find("SomeOtherSecretNetworkName") == std::string::npos);
    TEST_ASSERT_TRUE(lineNotFound.find("HomeNetworkSecretName") == std::string::npos);
    TEST_ASSERT_TRUE(lineNotFound.find("configured_ssid_found=false") != std::string::npos);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_placeholder_value_is_detected);
    RUN_TEST(test_non_placeholder_value_is_not_flagged);
    RUN_TEST(test_field_lengths_are_correct);
    RUN_TEST(test_config_summary_is_valid_only_when_both_fields_are_real_and_non_empty);
    RUN_TEST(test_credential_config_line_never_includes_secret_values);
    RUN_TEST(test_scan_finds_configured_ssid);
    RUN_TEST(test_scan_does_not_find_configured_ssid);
    RUN_TEST(test_empty_scan_reports_zero_networks_and_not_found);
    RUN_TEST(test_valid_scan_with_zero_networks_differs_from_failed_scan);
    RUN_TEST(test_failed_scan_status_persists_across_repeated_formatting);
    RUN_TEST(test_diagnose_credential_field_works_from_a_non_owning_view_without_copying);
    RUN_TEST(test_scan_line_never_includes_network_names);
    return UNITY_END();
}
