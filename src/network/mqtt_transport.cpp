#include "mqtt_transport.h"

namespace interbridge {

Esp32AwsIotTransport::Esp32AwsIotTransport(AwsIotConnectionConfig config, DeviceCredentialStore& credentials)
    : config_(std::move(config)), credentials_(credentials), connected_(false) {}

bool Esp32AwsIotTransport::connect(const std::string& clientId) {
    (void)clientId;
    // TODO: not implemented - no MQTT/TLS client wired up yet, and
    // config_.endpoint / credentials_ are not populated with real values.
    // See CONTEXT.md > Open Questions.
    return false;
}

void Esp32AwsIotTransport::disconnect() {
    connected_ = false;
}

bool Esp32AwsIotTransport::isConnected() const {
    return connected_;
}

bool Esp32AwsIotTransport::publish(const std::string& topic, const std::string& payload, MqttQos qos) {
    (void)topic;
    (void)payload;
    (void)qos;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

bool Esp32AwsIotTransport::subscribe(const std::string& topic, MqttQos qos, MqttMessageCallback callback) {
    (void)topic;
    (void)callback;
    (void)qos;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return false;
}

void Esp32AwsIotTransport::poll() {
    // TODO: not implemented. See CONTEXT.md > Open Questions.
}

FakeDeviceTransport::FakeDeviceTransport() : connected_(false), connectFailuresRemaining_(0) {}

bool FakeDeviceTransport::connect(const std::string& clientId) {
    if (connectFailuresRemaining_ > 0) {
        connectFailuresRemaining_--;
        connected_ = false;
        return false;
    }
    clientId_ = clientId;
    connected_ = true;
    return true;
}

void FakeDeviceTransport::disconnect() {
    connected_ = false;
}

bool FakeDeviceTransport::isConnected() const {
    return connected_;
}

bool FakeDeviceTransport::publish(const std::string& topic, const std::string& payload, MqttQos qos) {
    if (!connected_) {
        return false;
    }
    published_.push_back(PublishedMessage{topic, payload, qos});
    return true;
}

bool FakeDeviceTransport::subscribe(const std::string& topic, MqttQos qos, MqttMessageCallback callback) {
    (void)qos;
    if (!connected_) {
        return false;
    }
    subscriptions_.emplace_back(topic, std::move(callback));
    return true;
}

void FakeDeviceTransport::poll() {}

void FakeDeviceTransport::armConnectFailure(int timesToFail) {
    connectFailuresRemaining_ = timesToFail;
}

const std::vector<FakeDeviceTransport::PublishedMessage>& FakeDeviceTransport::publishedMessages() const {
    return published_;
}

const std::string& FakeDeviceTransport::lastClientId() const {
    return clientId_;
}

void FakeDeviceTransport::deliver(const std::string& topic, const std::string& payload) {
    for (const auto& [subscribedTopic, callback] : subscriptions_) {
        if (subscribedTopic == topic && callback) {
            callback(topic, payload);
        }
    }
}

} // namespace interbridge
