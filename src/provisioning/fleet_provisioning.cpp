#include "fleet_provisioning.h"

namespace interbridge {

std::optional<IKeyPairGenerator::KeyPair> Esp32KeyPairGenerator::generate(const std::string& commonName) {
    (void)commonName;
    // TODO: not implemented - no crypto library wired up yet.
    // See CONTEXT.md > Open Questions.
    return std::nullopt;
}

FakeKeyPairGenerator::FakeKeyPairGenerator()
    : result_(KeyPair{"fake-private-key", "fake-csr"}) {}

void FakeKeyPairGenerator::setResult(std::optional<KeyPair> result) {
    result_ = std::move(result);
}

std::optional<IKeyPairGenerator::KeyPair> FakeKeyPairGenerator::generate(const std::string& commonName) {
    (void)commonName;
    return result_;
}

std::optional<IFleetProvisioningTransport::CertificateResult> Esp32FleetProvisioningTransport::createCertificateFromCsr(
    const std::string& csrPem) {
    (void)csrPem;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return std::nullopt;
}

bool Esp32FleetProvisioningTransport::registerThing(const std::string& templateName,
                                                      const std::string& certificateOwnershipToken,
                                                      const std::string& deviceId) {
    (void)templateName;
    (void)certificateOwnershipToken;
    (void)deviceId;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

FakeFleetProvisioningTransport::FakeFleetProvisioningTransport()
    : createResult_(CertificateResult{"fake-cert", "fake-ownership-token"}),
      registerResult_(true),
      createCalls_(0),
      registerCalls_(0) {}

void FakeFleetProvisioningTransport::setCreateCertificateResult(std::optional<CertificateResult> result) {
    createResult_ = std::move(result);
}

void FakeFleetProvisioningTransport::setRegisterThingResult(bool result) {
    registerResult_ = result;
}

std::optional<IFleetProvisioningTransport::CertificateResult> FakeFleetProvisioningTransport::createCertificateFromCsr(
    const std::string& csrPem) {
    (void)csrPem;
    createCalls_++;
    return createResult_;
}

bool FakeFleetProvisioningTransport::registerThing(const std::string& templateName,
                                                     const std::string& certificateOwnershipToken,
                                                     const std::string& deviceId) {
    (void)templateName;
    (void)certificateOwnershipToken;
    (void)deviceId;
    registerCalls_++;
    return registerResult_;
}

int FakeFleetProvisioningTransport::createCertificateCalls() const {
    return createCalls_;
}

int FakeFleetProvisioningTransport::registerThingCalls() const {
    return registerCalls_;
}

FleetProvisioningCoordinator::FleetProvisioningCoordinator(IKeyPairGenerator& keyGen,
                                                             IFleetProvisioningTransport& transport,
                                                             DeviceCredentialStore& credentialStore,
                                                             std::string templateName)
    : keyGen_(keyGen), transport_(transport), credentialStore_(credentialStore), templateName_(std::move(templateName)) {}

FleetProvisioningResult FleetProvisioningCoordinator::provision(const std::string& deviceId) {
    auto keyPair = keyGen_.generate(deviceId);
    if (!keyPair.has_value()) {
        return FleetProvisioningResult::KeyGenerationFailed;
    }

    auto certResult = transport_.createCertificateFromCsr(keyPair->csrPem);
    if (!certResult.has_value()) {
        return FleetProvisioningResult::CertificateRequestFailed;
    }

    bool registered = transport_.registerThing(templateName_, certResult->certificateOwnershipToken, deviceId);
    if (!registered) {
        return FleetProvisioningResult::RegisterThingFailed;
    }

    credentialStore_.savePrivateKey(keyPair->privateKeyPem);
    credentialStore_.saveCertificate(certResult->certificatePem);

    return FleetProvisioningResult::Success;
}

} // namespace interbridge
