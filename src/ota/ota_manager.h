#pragma once

#include <optional>
#include <string>
#include <vector>

#include "firmware_validation.h"

namespace interbridge {

enum class OtaResult {
    Success,
    VersionRejected,
    DownloadFailed,
    HashMismatch,
    SignatureInvalid,
    InstallFailed,
    BootValidationFailed,
};
const char* toString(OtaResult result);

// The platform-specific half of OTA: downloading bytes into the inactive
// partition, installing/rebooting, and boot self-test/rollback. Modeled
// generically since real ESP32 dual-OTA-partition handling (the Arduino
// Update library / esp_ota_* APIs) is not implemented here.
class IOtaPlatform {
public:
    virtual ~IOtaPlatform() = default;

    // Downloads the firmware image from `url` (a short-lived presigned
    // HTTPS URL from the AWS IoT Job document) into the inactive
    // partition/staging area, returning the SHA-256 digest (hex) of what
    // was downloaded. Returns std::nullopt on any download failure.
    virtual std::optional<std::string> downloadAndHash(const std::string& url) = 0;

    // Marks the staged image as the one to boot next and reboots. On
    // real hardware, does not return if it succeeds.
    virtual bool installAndReboot() = 0;

    // Called after rebooting into the new image, before the previous
    // image's "valid" mark is cleared. Returning false must trigger
    // rollback to the previous image.
    virtual bool confirmBootOrRollback() = 0;
};

// Real ESP32 implementation. STUB: no HTTPS download client or ESP32 OTA
// partition handling has been wired up yet. See CONTEXT.md > Open
// Questions.
class Esp32OtaPlatform : public IOtaPlatform {
public:
    std::optional<std::string> downloadAndHash(const std::string& url) override;
    bool installAndReboot() override;
    bool confirmBootOrRollback() override;
};

// Test double: every step is independently configurable to
// succeed/fail, and calls are recorded so tests can assert the flow
// stopped at the right step (e.g. install/confirm never called after a
// download failure).
class FakeOtaPlatform : public IOtaPlatform {
public:
    FakeOtaPlatform();

    void setDownloadResult(std::optional<std::string> hexDigestOrNullopt);
    void setInstallResult(bool result);
    void setConfirmResult(bool result);

    std::optional<std::string> downloadAndHash(const std::string& url) override;
    bool installAndReboot() override;
    bool confirmBootOrRollback() override;

    int downloadCalls() const;
    int installCalls() const;
    int confirmCalls() const;
    const std::string& lastUrl() const;

private:
    std::optional<std::string> downloadResult_;
    bool installResult_;
    bool confirmResult_;
    int downloadCalls_;
    int installCalls_;
    int confirmCalls_;
    std::string lastUrl_;
};

// Orchestrates the OTA flow described in
// docs/communication-protocol.md > OTA Manager:
//   version check -> download+hash -> SHA-256 validation ->
//   signature validation -> install+reboot -> boot confirmation.
//
// This drives the flow synchronously for now, which is a simplification:
// a real download can take a long time and must not block intercom
// polling/state handling - see CONTEXT.md > Technical Debt for the
// asynchronous/state-machine-driven version this should become.
//
// The job document does not carry a signature field yet (see
// aws/jobs.h), so signature verification is currently always invoked
// with an empty signature - with DefaultFirmwareVerifier this makes
// every real OTA attempt fail at the signature step by design, honestly
// reflecting that signed OTA is not implemented.
class OtaCoordinator {
public:
    OtaCoordinator(IOtaPlatform& platform, IFirmwareVerifier& verifier, std::string currentVersion);

    OtaResult apply(const std::string& targetVersion, const std::string& url, const std::string& expectedSha256);

private:
    IOtaPlatform& platform_;
    IFirmwareVerifier& verifier_;
    std::string currentVersion_;
};

// Compares two "x.y.z" version strings numerically, component by
// component (missing components treated as 0). Returns true if `a` is
// strictly newer than `b`. Does not understand pre-release/build
// metadata suffixes - see CONTEXT.md > Known Limitations.
bool isNewerVersion(const std::string& a, const std::string& b);

} // namespace interbridge
