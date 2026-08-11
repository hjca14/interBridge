#include "ota_manager.h"

#include <sstream>
#include <vector>

namespace {
std::vector<int> parseVersionComponents(const std::string& version) {
    std::vector<int> components;
    std::stringstream ss(version);
    std::string part;
    while (std::getline(ss, part, '.')) {
        try {
            components.push_back(std::stoi(part));
        } catch (...) {
            components.push_back(0);
        }
    }
    return components;
}
} // namespace

namespace interbridge {

const char* toString(OtaResult result) {
    switch (result) {
        case OtaResult::Success: return "SUCCESS";
        case OtaResult::VersionRejected: return "VERSION_REJECTED";
        case OtaResult::DownloadFailed: return "DOWNLOAD_FAILED";
        case OtaResult::HashMismatch: return "HASH_MISMATCH";
        case OtaResult::SignatureInvalid: return "SIGNATURE_INVALID";
        case OtaResult::InstallFailed: return "INSTALL_FAILED";
        case OtaResult::BootValidationFailed: return "BOOT_VALIDATION_FAILED";
    }
    return "UNKNOWN_OTA_RESULT";
}

bool isNewerVersion(const std::string& a, const std::string& b) {
    std::vector<int> va = parseVersionComponents(a);
    std::vector<int> vb = parseVersionComponents(b);
    size_t n = va.size() > vb.size() ? va.size() : vb.size();
    for (size_t i = 0; i < n; i++) {
        int ca = i < va.size() ? va[i] : 0;
        int cb = i < vb.size() ? vb[i] : 0;
        if (ca != cb) {
            return ca > cb;
        }
    }
    return false; // equal versions are not "newer"
}

std::optional<std::string> Esp32OtaPlatform::downloadAndHash(const std::string& url) {
    (void)url;
    // TODO: not implemented - no HTTPS client wired up yet.
    // See CONTEXT.md > Open Questions.
    return std::nullopt;
}

bool Esp32OtaPlatform::installAndReboot() {
    // TODO: not implemented - ESP32 OTA partition handling not wired up.
    // See CONTEXT.md > Open Questions.
    return false;
}

bool Esp32OtaPlatform::confirmBootOrRollback() {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

FakeOtaPlatform::FakeOtaPlatform()
    : downloadResult_(std::string("fakehash")),
      installResult_(true),
      confirmResult_(true),
      downloadCalls_(0),
      installCalls_(0),
      confirmCalls_(0) {}

void FakeOtaPlatform::setDownloadResult(std::optional<std::string> hexDigestOrNullopt) {
    downloadResult_ = std::move(hexDigestOrNullopt);
}

void FakeOtaPlatform::setInstallResult(bool result) {
    installResult_ = result;
}

void FakeOtaPlatform::setConfirmResult(bool result) {
    confirmResult_ = result;
}

std::optional<std::string> FakeOtaPlatform::downloadAndHash(const std::string& url) {
    downloadCalls_++;
    lastUrl_ = url;
    return downloadResult_;
}

bool FakeOtaPlatform::installAndReboot() {
    installCalls_++;
    return installResult_;
}

bool FakeOtaPlatform::confirmBootOrRollback() {
    confirmCalls_++;
    return confirmResult_;
}

int FakeOtaPlatform::downloadCalls() const {
    return downloadCalls_;
}

int FakeOtaPlatform::installCalls() const {
    return installCalls_;
}

int FakeOtaPlatform::confirmCalls() const {
    return confirmCalls_;
}

const std::string& FakeOtaPlatform::lastUrl() const {
    return lastUrl_;
}

OtaCoordinator::OtaCoordinator(IOtaPlatform& platform, IFirmwareVerifier& verifier, std::string currentVersion)
    : platform_(platform), verifier_(verifier), currentVersion_(std::move(currentVersion)) {}

OtaResult OtaCoordinator::apply(const std::string& targetVersion, const std::string& url,
                                 const std::string& expectedSha256) {
    if (!isNewerVersion(targetVersion, currentVersion_)) {
        return OtaResult::VersionRejected;
    }

    auto downloaded = platform_.downloadAndHash(url);
    if (!downloaded.has_value()) {
        return OtaResult::DownloadFailed;
    }

    if (!verifier_.verifySha256(*downloaded, expectedSha256)) {
        return OtaResult::HashMismatch;
    }

    // No signature field is transported yet - see class-level comment.
    if (!verifier_.verifySignature(*downloaded, "")) {
        return OtaResult::SignatureInvalid;
    }

    if (!platform_.installAndReboot()) {
        return OtaResult::InstallFailed;
    }

    if (!platform_.confirmBootOrRollback()) {
        return OtaResult::BootValidationFailed;
    }

    return OtaResult::Success;
}

} // namespace interbridge
