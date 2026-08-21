#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../storage/credential_store.h"

namespace interbridge {

enum class MqttQos { AtMostOnce = 0, AtLeastOnce = 1 };

using MqttMessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

// AWS IoT connection parameters that are not decided yet (endpoint,
// region, root CA). Placeholders only - see CONTEXT.md > Open Questions.
// The root CA is configuration, not a secret, but is still left empty
// until embedded deliberately rather than hardcoding a value that could
// go stale.
struct AwsIotConnectionConfig {
    std::string endpoint; // e.g. "xxxxxxxxxxxxx-ats.iot.<region>.amazonaws.com"
    std::string region;
    std::string rootCaPem;
};

// Low-level MQTT 3.1.1/TLS transport. Deliberately narrow: connection
// lifecycle, publish, subscribe, poll. Message *meaning* (topics, JSON
// payloads, command dispatch) is layered on top by mqtt_topics.h,
// protocol/messages.h and protocol/command_handler.h.
//
// This does NOT expose a custom Last Will/availability mechanism - see
// docs/communication-protocol.md > AWS Connectivity Lifecycle: the
// backend is expected to use AWS IoT's own connectivity lifecycle
// events, not a device-side LWT topic.
class IDeviceTransport {
public:
    virtual ~IDeviceTransport() = default;

    virtual bool connect(const std::string& clientId) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual bool publish(const std::string& topic, const std::string& payload, MqttQos qos) = 0;
    virtual bool subscribe(const std::string& topic, MqttMessageCallback callback) = 0;

    // Must be called regularly (non-blocking) to process incoming
    // messages and keep-alive traffic.
    virtual void poll() = 0;
};

// Real ESP32/AWS IoT implementation. STUB: no MQTT 3.1.1/TLS client has
// been wired up yet (e.g. an ESP32 MQTT library plus mbedTLS configured
// with the AWS IoT root CA and this device's X.509 client
// certificate/key from DeviceCredentialStore), and there is no AWS IoT
// account/endpoint to connect to yet. Every method is a placeholder. See
// CONTEXT.md > Open Questions.
class Esp32AwsIotTransport : public IDeviceTransport {
public:
    Esp32AwsIotTransport(AwsIotConnectionConfig config, DeviceCredentialStore& credentials);

    bool connect(const std::string& clientId) override;
    void disconnect() override;
    bool isConnected() const override;
    bool publish(const std::string& topic, const std::string& payload, MqttQos qos) override;
    bool subscribe(const std::string& topic, MqttMessageCallback callback) override;
    void poll() override;

private:
    AwsIotConnectionConfig config_;
    DeviceCredentialStore& credentials_;
    bool connected_;
};

// In-memory fake used for native tests and local development without a
// real broker. Records every publish() call, tracks subscriptions, and
// lets tests simulate an incoming message via deliver(). Disconnect clears
// subscriptions so reconnect logic must subscribe again. Use
// armConnectFailure() to make the next N connect() calls fail, for
// deterministic reconnect-logic tests.
class FakeDeviceTransport : public IDeviceTransport {
public:
    struct PublishedMessage {
        std::string topic;
        std::string payload;
        MqttQos qos;
    };

    FakeDeviceTransport();

    bool connect(const std::string& clientId) override;
    void disconnect() override;
    bool isConnected() const override;
    bool publish(const std::string& topic, const std::string& payload, MqttQos qos) override;
    bool subscribe(const std::string& topic, MqttMessageCallback callback) override;
    void poll() override;

    void armConnectFailure(int timesToFail);
    void armPublishFailure(int timesToFail);
    void armPublishFailureOnCall(int callNumber);
    const std::vector<PublishedMessage>& publishedMessages() const;
    size_t subscriptionCount() const;
    const std::string& lastClientId() const;
    void deliver(const std::string& topic, const std::string& payload);

private:
    bool connected_;
    int connectFailuresRemaining_;
    int publishFailuresRemaining_;
    int publishCallCount_;
    int failPublishCall_;
    std::string clientId_;
    std::vector<PublishedMessage> published_;
    std::vector<std::pair<std::string, MqttMessageCallback>> subscriptions_;
};

} // namespace interbridge
