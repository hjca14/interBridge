#pragma once

#include <optional>
#include <string>

#include "../storage/credential_store.h"

namespace interbridge {

// Locally generates a keypair and a CSR for it. Real implementation
// needs an on-device crypto library (e.g. mbedtls) - not wired up yet.
// The private key must never leave the device (see
// docs/communication-protocol.md > Device Credential Storage) - only the
// CSR (public) is ever sent to AWS.
class IKeyPairGenerator {
public:
    struct KeyPair {
        std::string privateKeyPem;
        std::string csrPem;
    };

    virtual ~IKeyPairGenerator() = default;
    virtual std::optional<KeyPair> generate(const std::string& commonName) = 0;
};

// Real ESP32 implementation. STUB: no crypto library has been wired up
// for on-device keypair/CSR generation yet. See CONTEXT.md > Open
// Questions.
class Esp32KeyPairGenerator : public IKeyPairGenerator {
public:
    std::optional<KeyPair> generate(const std::string& commonName) override;
};

class FakeKeyPairGenerator : public IKeyPairGenerator {
public:
    FakeKeyPairGenerator();
    void setResult(std::optional<KeyPair> result);
    std::optional<KeyPair> generate(const std::string& commonName) override;

private:
    std::optional<KeyPair> result_;
};

// AWS IoT Fleet Provisioning by trusted user: CreateCertificateFromCsr +
// RegisterThing, over MQTT topics reserved by AWS (see
// network/mqtt_topics.h) using a temporary claim credential - NOT the
// permanent device identity. Modeled as its own transport interface
// because it's a one-time bootstrap flow with its own request/response
// shape, distinct from IDeviceTransport's generic pub/sub.
class IFleetProvisioningTransport {
public:
    struct CertificateResult {
        std::string certificatePem;
        std::string certificateOwnershipToken;
    };

    virtual ~IFleetProvisioningTransport() = default;

    virtual std::optional<CertificateResult> createCertificateFromCsr(const std::string& csrPem) = 0;
    virtual bool registerThing(const std::string& templateName, const std::string& certificateOwnershipToken,
                                const std::string& deviceId) = 0;
};

// Real ESP32/AWS implementation. STUB: requires a working MQTT/TLS
// connection using a temporary claim credential that does not exist yet
// (see CONTEXT.md > Open Questions), plus AWS infrastructure (template
// name, account, region).
class Esp32FleetProvisioningTransport : public IFleetProvisioningTransport {
public:
    std::optional<CertificateResult> createCertificateFromCsr(const std::string& csrPem) override;
    bool registerThing(const std::string& templateName, const std::string& certificateOwnershipToken,
                        const std::string& deviceId) override;
};

class FakeFleetProvisioningTransport : public IFleetProvisioningTransport {
public:
    FakeFleetProvisioningTransport();

    void setCreateCertificateResult(std::optional<CertificateResult> result);
    void setRegisterThingResult(bool result);

    std::optional<CertificateResult> createCertificateFromCsr(const std::string& csrPem) override;
    bool registerThing(const std::string& templateName, const std::string& certificateOwnershipToken,
                        const std::string& deviceId) override;

    int createCertificateCalls() const;
    int registerThingCalls() const;

private:
    std::optional<CertificateResult> createResult_;
    bool registerResult_;
    int createCalls_;
    int registerCalls_;
};

enum class FleetProvisioningResult {
    Success,
    KeyGenerationFailed,
    CertificateRequestFailed,
    RegisterThingFailed,
};

// Orchestrates the CSR flow end-to-end, per
// docs/communication-protocol.md > Fleet Provisioning by Trusted User:
// generate keypair locally -> generate CSR -> CreateCertificateFromCsr
// -> RegisterThing -> store the permanent certificate -> the temporary
// claim credential used to authenticate this flow is never persisted by
// this class (it's the caller's responsibility to use a short-lived,
// memory-only claim credential for whatever transport backs
// IFleetProvisioningTransport).
class FleetProvisioningCoordinator {
public:
    FleetProvisioningCoordinator(IKeyPairGenerator& keyGen, IFleetProvisioningTransport& transport,
                                  DeviceCredentialStore& credentialStore, std::string templateName);

    FleetProvisioningResult provision(const std::string& deviceId);

private:
    IKeyPairGenerator& keyGen_;
    IFleetProvisioningTransport& transport_;
    DeviceCredentialStore& credentialStore_;
    std::string templateName_;
};

} // namespace interbridge
