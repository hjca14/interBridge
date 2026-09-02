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

// ESP32 adapter. In the isolated Phase 3C.1 build it uses Arduino-ESP32's
// official WiFiProv wrapper, which delegates to ESP-IDF Wi-Fi Provisioning
// Manager and Protocomm. Other compositions retain the fail-closed adapter
// until production PoP storage and lifecycle wiring are designed.
class Esp32BleProvisioning : public IBleProvisioning {
public:
    explicit Esp32BleProvisioning(BleSecurityMode requestedMode = BleSecurityMode::Security2);

    bool startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    bool isSessionActive() const override;
    BleSecurityMode securityMode() const override;
    std::optional<WifiCredentialsPayload> pollReceivedCredentials() override;

    // Forwarded by the Arduino system-event callback in the isolated DEV
    // composition. Arguments are intentionally metadata-only.
    void notifySecureSessionEstablished();
    void notifyDisconnected();
    void notifyCredentials(const std::string& ssid, const std::string& password);
    void notifyFailure();

private:
    BleSecurityMode securityMode_;
    bool advertising_;
    bool sessionActive_;
    std::optional<WifiCredentialsPayload> receivedCredentials_;
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
    void setStartResult(bool result);
    const std::string& lastProofOfPossession() const;
    const BleAdvertisementInfo& lastAdvertisementInfo() const;

private:
    BleSecurityMode securityMode_;
    bool advertising_;
    bool sessionActive_;
    bool startResult_;
    std::string lastPop_;
    BleAdvertisementInfo lastInfo_;
    std::optional<WifiCredentialsPayload> queuedCredentials_;
};

} // namespace interbridge
