#include "firmware_validation.h"

#include <algorithm>
#include <cctype>

namespace {
std::string toLower(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
} // namespace

namespace interbridge {

bool DefaultFirmwareVerifier::verifySha256(const std::string& computedHexDigest, const std::string& expectedHexDigest) {
    return toLower(computedHexDigest) == toLower(expectedHexDigest);
}

bool DefaultFirmwareVerifier::verifySignature(const std::string& firmwareDigestHex, const std::string& signature) {
    (void)firmwareDigestHex;
    (void)signature;
    // TODO: not implemented - no signing scheme/public key chosen yet.
    // See CONTEXT.md > Open Questions. Fails closed deliberately.
    return false;
}

FakeFirmwareVerifier::FakeFirmwareVerifier() : shaResult_(true), signatureResult_(true) {}

void FakeFirmwareVerifier::setShaResult(bool result) {
    shaResult_ = result;
}

void FakeFirmwareVerifier::setSignatureResult(bool result) {
    signatureResult_ = result;
}

bool FakeFirmwareVerifier::verifySha256(const std::string& computedHexDigest, const std::string& expectedHexDigest) {
    (void)computedHexDigest;
    (void)expectedHexDigest;
    return shaResult_;
}

bool FakeFirmwareVerifier::verifySignature(const std::string& firmwareDigestHex, const std::string& signature) {
    (void)firmwareDigestHex;
    (void)signature;
    return signatureResult_;
}

} // namespace interbridge
