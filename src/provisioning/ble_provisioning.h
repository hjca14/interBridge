#pragma once

#include <cstdint>
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

    // Requests the start of BLE advertising for provisioning with the
    // given (non-secret) advertisement metadata, securing the session
    // with proofOfPossession. Real PoP generation/storage is not
    // implemented yet - see provisioning_manager.h. A true return means
    // only that the request was made with valid inputs - see
    // isAdvertising()'s doc comment for why that is not the same as
    // advertising being confirmed active.
    virtual bool startAdvertising(const BleAdvertisementInfo& info, const std::string& proofOfPossession) = 0;
    virtual void stopAdvertising() = 0;
    // True once BLE advertising is confirmed active. A bench run showed
    // this matters: the real adapter's underlying WiFiProv call has no
    // synchronous success/failure return, so a successful
    // startAdvertising() call alone must never be treated as advertising
    // being active - see Esp32BleProvisioning::notifyAdvertisingStarted()
    // and BleOnboardingWindow below, which exist specifically to keep
    // that request/confirmation distinction fail-closed.
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
    //
    // notifyAdvertisingStarted() must only be called on a real, observed
    // ARDUINO_EVENT_PROV_START - never merely because startAdvertising()
    // returned true. See isAdvertising()'s doc comment.
    void notifyAdvertisingStarted();
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

// Tracks the "start requested -> confirmed active -> window closed"
// lifecycle for one BLE onboarding attempt. Deliberately
// hardware-independent (nowMs is caller-supplied, exactly like
// DevMqttSmokeState in src/dev/mqtt_smoke_state.h) so this state machine
// is natively unit-testable without any Arduino/ESP-IDF headers.
//
// This exists because WiFiProv.beginProvision() (see
// Esp32BleProvisioning::startAdvertising()) has no synchronous success/
// failure return. A real bench run showed the consequence: the previous
// isolated DEV entry point opened its 5-minute window the instant the
// start request was made, so a device that never actually started BLE
// advertising still printed a plausible-looking "onboarding window
// closed: timeout" - no evidence it had ever tried. requestStart()
// therefore only arms a short confirmation deadline; only confirmStart()
// (driven by the real ARDUINO_EVENT_PROV_START event) opens the actual
// onboarding window. If confirmation never arrives, update() reports
// StartNotConfirmed instead of ever reporting a timeout for a window
// that never opened - fail-closed by construction. See
// docs/ble-onboarding.md's "Physical validation" section.
enum class BleOnboardingWindowEvent { None, StartNotConfirmed, WindowTimedOut };

class BleOnboardingWindow {
public:
    BleOnboardingWindow(uint32_t confirmationTimeoutMs, uint32_t windowMs);

    // Call once the start request has been made (e.g. right after a
    // successful Esp32BleProvisioning::startAdvertising() call).
    void requestStart(uint32_t nowMs);
    // Call when the real start confirmation event arrives
    // (ARDUINO_EVENT_PROV_START). No-op if requestStart() was never
    // called, or the attempt already resolved (confirmed, or its
    // confirmation deadline already passed) - a late confirmation must
    // never resurrect an attempt update() has already closed.
    void confirmStart(uint32_t nowMs);
    // Closes the window immediately regardless of its own timeout (e.g.
    // on ARDUINO_EVENT_PROV_END).
    void close();

    // Call once per loop tick. Returns StartNotConfirmed the first tick
    // it observes the confirmation deadline reached while still awaiting
    // confirmation, or WindowTimedOut the first tick it observes the
    // window deadline reached while open - each fires exactly once per
    // attempt, never repeated on a later tick.
    BleOnboardingWindowEvent update(uint32_t nowMs);

    bool isAwaitingConfirmation() const;
    bool isOpen() const;

private:
    uint32_t confirmationTimeoutMs_;
    uint32_t windowMs_;
    bool awaitingConfirmation_ = false;
    bool open_ = false;
    uint32_t confirmationDeadlineMs_ = 0;
    uint32_t windowDeadlineMs_ = 0;
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
