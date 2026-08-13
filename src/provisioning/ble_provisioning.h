#pragma once

#include <optional>
#include <string>

namespace interbridge {

struct WifiCredentialsPayload {
    std::string ssid;
    std::string password;
};

// Protocomm security mode used to secure the BLE provisioning session.
// Security2 is preferred when the pinned ESP-IDF version supports it
// cleanly; Security1 is an acceptable production fallback. There is
// deliberately NO plaintext/"None" value in this enum - an
// implementation must never be able to silently downgrade to an
// insecure session by construction, not just by convention. See
// docs/communication-protocol.md > BLE Provisioning Framework.
enum class BleSecurityMode { Security1, Security2 };

// Non-secret metadata advertised while provisioning is active, per
// docs/communication-protocol.md > BLE Discovery. Centralized here so no
// other module builds this ad hoc.
struct BleAdvertisementInfo {
    std::string deviceName;         // e.g. "InterBridge-A91C"
    std::string deviceIdentityHint; // the short fragment used to build deviceName, e.g. "A91C"
    bool provisioningAvailable = true;
};

// Builds the advertisement model from device_id: the human-visible
// suffix is the last 4 hex characters of device_id, uppercased (e.g.
// device_id "ib-...a91c" -> deviceName "InterBridge-A91C"). Contains no
// secret material (no setup_code, PoP, or credentials) - only enough for
// the app to tell compatible, nearby devices apart.
BleAdvertisementInfo buildBleAdvertisementInfo(const std::string& deviceId);

// BLE-based Wi-Fi provisioning transport - the primary onboarding path
// (see docs/communication-protocol.md > Onboarding). The production
// design intent is ESP-IDF's Unified Provisioning (the wifi_provisioning
// component, Protocomm, a unique high-entropy Proof-of-Possession per
// device) - NOT a hand-rolled BLE GATT service. Real BLE service/
// characteristic UUIDs are not decided yet - see CONTEXT.md > Open
// Questions.
class IBleProvisioning {
public:
    virtual ~IBleProvisioning() = default;

    // Starts BLE advertising for provisioning with the given (non-secret)
    // advertisement metadata, securing the session with
    // proofOfPossession. Real PoP generation/storage is not implemented
    // yet - see provisioning_manager.h.
    virtual bool startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) = 0;
    virtual void stopAdvertising() = 0;
    virtual bool isAdvertising() const = 0;

    // True once a central (the app) has established a secure session.
    // Real BLE peripherals typically stop advertising once connected -
    // see provisioning_manager.h for how this drives state transitions.
    virtual bool isSessionActive() const = 0;

    // The Protocomm security mode this instance is configured to use -
    // see BleSecurityMode.
    virtual BleSecurityMode securityMode() const = 0;

    // Non-blocking poll for credentials received over the provisioning
    // channel.
    virtual std::optional<WifiCredentialsPayload> pollReceivedCredentials() = 0;
};

// Real ESP32 implementation. STUB, and deliberately so: this firmware
// currently targets `framework = arduino` (see platformio.ini). The
// ESP-IDF Unified Provisioning component that
// docs/communication-protocol.md calls for (wifi_provisioning +
// protocomm) is ESP-IDF-native; arduino-esp32 does not expose it through
// the Arduino API surface. A real implementation needs either (a)
// switching the esp32-c3 environment to `framework = espidf` (or a mixed
// `arduino, espidf` framework), or (b) a hand-rolled BLE GATT
// provisioning service on top of arduino-esp32's BLE library - which
// would NOT be ESP-IDF Unified Provisioning and would need its own
// security review before being called equivalent, and would need its own
// plan for reaching Security2 parity. Neither has been done; every
// method here is a placeholder. See CONTEXT.md > Open Questions.
class Esp32BleProvisioning : public IBleProvisioning {
public:
    explicit Esp32BleProvisioning(BleSecurityMode requestedMode = BleSecurityMode::Security2);

    bool startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    bool isSessionActive() const override;
    BleSecurityMode securityMode() const override;
    std::optional<WifiCredentialsPayload> pollReceivedCredentials() override;

private:
    BleSecurityMode securityMode_;
    bool advertising_;
};

// Test double: lets a test simulate a session starting and credentials
// arriving over BLE via setSessionActive()/injectCredentials().
class FakeBleProvisioning : public IBleProvisioning {
public:
    explicit FakeBleProvisioning(BleSecurityMode mode = BleSecurityMode::Security2);

    bool startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    bool isSessionActive() const override;
    BleSecurityMode securityMode() const override;
    std::optional<WifiCredentialsPayload> pollReceivedCredentials() override;

    void injectCredentials(const WifiCredentialsPayload& credentials);
    void setSessionActive(bool active);
    const std::string& lastProofOfPossession() const;
    const BleAdvertisementInfo& lastAdvertisementInfo() const;

private:
    BleSecurityMode securityMode_;
    bool advertising_;
    bool sessionActive_;
    std::string lastPop_;
    BleAdvertisementInfo lastInfo_;
    std::optional<WifiCredentialsPayload> queuedCredentials_;
};

} // namespace interbridge
