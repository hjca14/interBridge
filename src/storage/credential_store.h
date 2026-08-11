#pragma once

#include <optional>
#include <string>

#include "persistent_store.h"

namespace interbridge {

// Isolates access to device credential material (the AWS IoT certificate
// and private key) behind a narrow, purpose-built API instead of letting
// arbitrary code read/write those keys through IPersistentStore's
// generic get/set. This keeps "touch the raw private key" a single,
// auditable code path - see docs/communication-protocol.md > Device
// Credential Storage.
//
// Real on-device keypair generation (private key never leaves the
// device) is NOT implemented here - see provisioning/fleet_provisioning.h
// for the CSR flow, which is stubbed pending a chosen crypto library.
class DeviceCredentialStore {
public:
    explicit DeviceCredentialStore(IPersistentStore& store);

    bool hasCertificate() const;
    std::optional<std::string> loadCertificate() const;
    void saveCertificate(const std::string& pemCertificate);

    bool hasPrivateKey() const;
    // Real private key material. Callers must never log the returned
    // value - use describeForLogging() when only a status line is
    // needed.
    std::optional<std::string> loadPrivateKey() const;
    void savePrivateKey(const std::string& pemPrivateKey);

    // Erases both certificate and private key.
    void clear();

    // Safe for logs: reports presence/length only, never key material.
    std::string describeForLogging() const;

private:
    IPersistentStore& store_;
};

} // namespace interbridge
