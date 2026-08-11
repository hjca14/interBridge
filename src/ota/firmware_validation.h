#pragma once

#include <string>

namespace interbridge {

// Verifies downloaded firmware integrity (SHA-256) and authenticity
// (digital signature) before install.
class IFirmwareVerifier {
public:
    virtual ~IFirmwareVerifier() = default;

    // Compares an already-computed digest (hex) against the expected
    // sha256 from the OTA job document.
    virtual bool verifySha256(const std::string& computedHexDigest, const std::string& expectedHexDigest) = 0;

    // Verifies a firmware image's digital signature. NOT implemented for
    // any real signing scheme yet - see DefaultFirmwareVerifier.
    virtual bool verifySignature(const std::string& firmwareDigestHex, const std::string& signature) = 0;
};

// Real implementation. SHA-256 comparison is implemented (case-
// insensitive hex compare; the actual digest is computed during download
// - see ota_manager.h). Signature verification is a STUB that always
// fails closed (returns false): no signing scheme or public key has been
// chosen yet. This means real OTA cannot currently complete end-to-end -
// see CONTEXT.md > Open Questions. Fail-closed is deliberate: an
// unimplemented authenticity check must never be treated as "verified".
class DefaultFirmwareVerifier : public IFirmwareVerifier {
public:
    bool verifySha256(const std::string& computedHexDigest, const std::string& expectedHexDigest) override;
    bool verifySignature(const std::string& firmwareDigestHex, const std::string& signature) override;
};

// Test double: both checks configurable to pass/fail, so OtaCoordinator
// can be tested end-to-end without a real signing scheme.
class FakeFirmwareVerifier : public IFirmwareVerifier {
public:
    FakeFirmwareVerifier();

    void setShaResult(bool result);
    void setSignatureResult(bool result);

    bool verifySha256(const std::string& computedHexDigest, const std::string& expectedHexDigest) override;
    bool verifySignature(const std::string& firmwareDigestHex, const std::string& signature) override;

private:
    bool shaResult_;
    bool signatureResult_;
};

} // namespace interbridge
