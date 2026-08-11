#pragma once

#include <optional>
#include <string>

namespace interbridge {

struct WifiCredentialsPayload {
    std::string ssid;
    std::string password;
};

// BLE-based Wi-Fi provisioning transport. The production design intent
// (see docs/communication-protocol.md > BLE Provisioning) is ESP-IDF's
// Unified Provisioning (the wifi_provisioning component, Protocomm
// Security1, a unique high-entropy Proof-of-Possession per device) - NOT
// a hand-rolled BLE GATT service. Real BLE service/characteristic UUIDs
// are not decided yet - see CONTEXT.md > Open Questions.
class IBleProvisioning {
public:
    virtual ~IBleProvisioning() = default;

    // Starts BLE advertising for provisioning, using the given
    // Proof-of-Possession. Real PoP generation/storage is not
    // implemented yet - see provisioning_manager.h.
    virtual bool startAdvertising(const std::string& proofOfPossession) = 0;
    virtual void stopAdvertising() = 0;
    virtual bool isAdvertising() const = 0;

    // Non-blocking poll for credentials received over the provisioning
    // channel.
    virtual std::optional<WifiCredentialsPayload> pollReceivedCredentials() = 0;
};

// Real ESP32 implementation. STUB, and deliberately so: this firmware
// currently targets `framework = arduino` (see platformio.ini). The
// ESP-IDF Unified Provisioning component that
// docs/communication-protocol.md calls for (wifi_provisioning +
// protocomm + Security1) is ESP-IDF-native; arduino-esp32 does not
// expose it through the Arduino API surface. A real implementation needs
// either (a) switching the esp32-c3 environment to `framework = espidf`
// (or a mixed `arduino, espidf` framework), or (b) a hand-rolled BLE GATT
// provisioning service on top of arduino-esp32's BLE library - which
// would NOT be ESP-IDF Unified Provisioning and would need its own
// security review before being called equivalent. Neither has been done;
// every method here is a placeholder. See CONTEXT.md > Open Questions.
class Esp32BleProvisioning : public IBleProvisioning {
public:
    Esp32BleProvisioning();

    bool startAdvertising(const std::string& proofOfPossession) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    std::optional<WifiCredentialsPayload> pollReceivedCredentials() override;

private:
    bool advertising_;
};

// Test double: lets a test simulate credentials arriving over BLE via
// injectCredentials().
class FakeBleProvisioning : public IBleProvisioning {
public:
    FakeBleProvisioning();

    bool startAdvertising(const std::string& proofOfPossession) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    std::optional<WifiCredentialsPayload> pollReceivedCredentials() override;

    void injectCredentials(const WifiCredentialsPayload& credentials);
    const std::string& lastProofOfPossession() const;

private:
    bool advertising_;
    std::string lastPop_;
    std::optional<WifiCredentialsPayload> queuedCredentials_;
};

} // namespace interbridge
