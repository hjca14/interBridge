#include "credential_store.h"

namespace {
constexpr const char* kCertificateKey = "aws_certificate_pem";
constexpr const char* kPrivateKeyKey = "aws_private_key_pem";
} // namespace

namespace interbridge {

DeviceCredentialStore::DeviceCredentialStore(IPersistentStore& store) : store_(store) {}

bool DeviceCredentialStore::hasCertificate() const {
    return store_.has(kCertificateKey);
}

std::optional<std::string> DeviceCredentialStore::loadCertificate() const {
    return store_.get(kCertificateKey);
}

void DeviceCredentialStore::saveCertificate(const std::string& pemCertificate) {
    store_.set(kCertificateKey, pemCertificate);
}

bool DeviceCredentialStore::hasPrivateKey() const {
    return store_.has(kPrivateKeyKey);
}

std::optional<std::string> DeviceCredentialStore::loadPrivateKey() const {
    return store_.get(kPrivateKeyKey);
}

void DeviceCredentialStore::savePrivateKey(const std::string& pemPrivateKey) {
    store_.set(kPrivateKeyKey, pemPrivateKey);
}

void DeviceCredentialStore::clear() {
    store_.remove(kCertificateKey);
    store_.remove(kPrivateKeyKey);
}

std::string DeviceCredentialStore::describeForLogging() const {
    std::string description = "certificate=";
    description += hasCertificate() ? "present" : "absent";
    description += " private_key=";
    description += hasPrivateKey() ? "present" : "absent";
    return description;
}

} // namespace interbridge
