#include <unity.h>

#include "../../src/ota/firmware_validation.h"
#include "../../src/ota/ota_manager.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_is_newer_version() {
    TEST_ASSERT_TRUE(isNewerVersion("0.2.0", "0.1.0"));
    TEST_ASSERT_TRUE(isNewerVersion("1.0.0", "0.9.9"));
    TEST_ASSERT_FALSE(isNewerVersion("0.1.0", "0.1.0"));
    TEST_ASSERT_FALSE(isNewerVersion("0.1.0", "0.2.0"));
    TEST_ASSERT_TRUE(isNewerVersion("0.1.1", "0.1"));
}

void test_success_path() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("abc123"));
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::Success), static_cast<int>(result));
    TEST_ASSERT_EQUAL(1, platform.downloadCalls());
    TEST_ASSERT_EQUAL(1, platform.installCalls());
    TEST_ASSERT_EQUAL(1, platform.confirmCalls());
}

void test_version_not_newer_is_rejected_before_download() {
    FakeOtaPlatform platform;
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.2.0");

    OtaResult result = ota.apply("0.1.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::VersionRejected), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, platform.downloadCalls());
}

void test_download_failure_stops_before_install() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::nullopt);
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::DownloadFailed), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, platform.installCalls());
}

void test_hash_mismatch_stops_before_install() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("actual-hash"));
    FakeFirmwareVerifier verifier;
    verifier.setShaResult(false);
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "expected-hash");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::HashMismatch), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, platform.installCalls());
}

void test_signature_invalid_stops_before_install() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("abc123"));
    FakeFirmwareVerifier verifier;
    verifier.setSignatureResult(false);
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::SignatureInvalid), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, platform.installCalls());
}

void test_install_failure_stops_before_confirm() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("abc123"));
    platform.setInstallResult(false);
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::InstallFailed), static_cast<int>(result));
    TEST_ASSERT_EQUAL(0, platform.confirmCalls());
}

void test_boot_validation_failure_reports_rollback_needed() {
    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("abc123"));
    platform.setConfirmResult(false);
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");

    OtaResult result = ota.apply("0.2.0", "https://example.invalid/fw.bin", "abc123");

    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::BootValidationFailed), static_cast<int>(result));
}

void test_default_verifier_signature_check_fails_closed() {
    // DefaultFirmwareVerifier has no real signing scheme implemented yet
    // and must never report success for something it cannot verify.
    DefaultFirmwareVerifier verifier;
    TEST_ASSERT_FALSE(verifier.verifySignature("anydigest", "anysignature"));
}

void test_default_verifier_sha256_is_case_insensitive_compare() {
    DefaultFirmwareVerifier verifier;
    TEST_ASSERT_TRUE(verifier.verifySha256("ABC123", "abc123"));
    TEST_ASSERT_FALSE(verifier.verifySha256("abc123", "abc124"));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_is_newer_version);
    RUN_TEST(test_success_path);
    RUN_TEST(test_version_not_newer_is_rejected_before_download);
    RUN_TEST(test_download_failure_stops_before_install);
    RUN_TEST(test_hash_mismatch_stops_before_install);
    RUN_TEST(test_signature_invalid_stops_before_install);
    RUN_TEST(test_install_failure_stops_before_confirm);
    RUN_TEST(test_boot_validation_failure_reports_rollback_needed);
    RUN_TEST(test_default_verifier_signature_check_fails_closed);
    RUN_TEST(test_default_verifier_sha256_is_case_insensitive_compare);
    return UNITY_END();
}
