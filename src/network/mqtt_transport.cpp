#include "mqtt_transport.h"

#include "../core/logger.h"
#include <cctype>

#ifdef ARDUINO
#include <MQTT.h>
#include <WiFiClientSecure.h>
#endif

namespace interbridge {
namespace {
#ifdef ARDUINO
constexpr uint32_t kTlsHandshakeTimeoutSeconds = 10;

class ArduinoMqttClient final : public IMqttClient {
public:
  ArduinoMqttClient() : mqtt_(9216) {}
  bool configureTls(const std::string &endpoint, uint16_t port,
                    const std::string &ca, const std::string &cert,
                    const std::string &key, uint16_t keepAlive,
                    uint16_t timeout) override {
    tls_.setCACert(ca.c_str());
    tls_.setCertificate(cert.c_str());
    tls_.setPrivateKey(key.c_str());
    // WiFiClientSecure otherwise inherits a long/default stream timeout. Keep
    // individual TLS reads and writes bounded so the main loop remains live.
    tls_.setTimeout(timeout);
    // TLS negotiation can legitimately take several seconds on a congested
    // link. Keep it bounded without applying the shorter stream timeout to the
    // complete AWS IoT handshake.
    tls_.setHandshakeTimeout(kTlsHandshakeTimeoutSeconds);
    mqtt_.begin(endpoint.c_str(), port, tls_);
    mqtt_.setOptions(keepAlive, true, timeout);
    return true;
  }
  void setMessageCallback(MqttMessageCallback callback) override {
    callback_ = std::move(callback);
    mqtt_.onMessage([this](String &topic, String &payload) {
      if (callback_)
        callback_(std::string(topic.c_str(), topic.length()),
                  std::string(payload.c_str(), payload.length()));
    });
  }
  bool connect(const std::string &id) override {
    return mqtt_.connect(id.c_str());
  }
  void disconnect() override { mqtt_.disconnect(); }
  bool connected() override { return mqtt_.connected(); }
  bool publish(const std::string &topic, const std::string &payload,
               MqttQos qos, bool retain) override {
    return mqtt_.publish(topic.c_str(), payload.c_str(), retain,
                         static_cast<int>(qos));
  }
  bool subscribe(const std::string &topic, MqttQos qos) override {
    return mqtt_.subscribe(topic.c_str(), static_cast<int>(qos));
  }
  void poll() override { mqtt_.loop(); }

private:
  WiFiClientSecure tls_;
  MQTTClient mqtt_;
  MqttMessageCallback callback_;
};
#else
class ArduinoMqttClient final : public IMqttClient {
public:
  bool configureTls(const std::string &, uint16_t, const std::string &,
                    const std::string &, const std::string &, uint16_t,
                    uint16_t) override {
    return false;
  }
  void setMessageCallback(MqttMessageCallback) override {}
  bool connect(const std::string &) override { return false; }
  void disconnect() override {}
  bool connected() override { return false; }
  bool publish(const std::string &, const std::string &, MqttQos,
               bool) override {
    return false;
  }
  bool subscribe(const std::string &, MqttQos) override { return false; }
  void poll() override {}
};
#endif
bool endsWith(const std::string &value, const std::string &suffix) {
  return value.size() > suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}
} // namespace

Esp32AwsIotTransport::Esp32AwsIotTransport(AwsIotConnectionConfig config,
                                           DeviceCredentialStore &credentials)
    : config_(std::move(config)), credentials_(credentials),
      ownedClient_(new ArduinoMqttClient()), client_(ownedClient_.get()) {}
Esp32AwsIotTransport::Esp32AwsIotTransport(AwsIotConnectionConfig config,
                                           DeviceCredentialStore &credentials,
                                           IMqttClient &client)
    : config_(std::move(config)), credentials_(credentials), client_(&client) {}
Esp32AwsIotTransport::~Esp32AwsIotTransport() = default;

bool Esp32AwsIotTransport::validEndpoint(const std::string &endpoint) {
  if (!endsWith(endpoint, ".amazonaws.com") ||
      endpoint.find("-ats.iot.") == std::string::npos ||
      endpoint.find('/') != std::string::npos ||
      endpoint.find(':') != std::string::npos)
    return false;
  for (char c : endpoint)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-'))
      return false;
  return true;
}
bool Esp32AwsIotTransport::validDeviceId(const std::string &id) {
  if (id.size() != 35 || id.compare(0, 3, "ib-") != 0)
    return false;
  for (size_t i = 3; i < id.size(); ++i)
    if (!std::isdigit(id[i]) && !(id[i] >= 'a' && id[i] <= 'f'))
      return false;
  return true;
}
bool Esp32AwsIotTransport::connect(const std::string &clientId) {
  if (!validEndpoint(config_.endpoint) || !validDeviceId(clientId) ||
      config_.rootCaPem.empty() || !credentials_.hasCertificate() ||
      !credentials_.hasPrivateKey()) {
    Logger::error(
        "AWS IoT connection configuration/identity unavailable or invalid");
    return false;
  }
  const auto cert = credentials_.loadCertificate();
  const auto key = credentials_.loadPrivateKey();
  if (!cert || cert->empty() || !key || key->empty()) {
    Logger::error("AWS IoT device credentials unavailable");
    return false;
  }
  certificatePem_ = *cert;
  privateKeyPem_ = *key;
  client_->setMessageCallback(
      [this](const std::string &topic, const std::string &payload) {
        if (callback_)
          callback_(topic, payload);
      });
  if (!client_->configureTls(config_.endpoint, config_.port, config_.rootCaPem,
                             certificatePem_, privateKeyPem_,
                             config_.keepAliveSeconds, config_.timeoutMs)) {
    Logger::error("AWS IoT TLS configuration failed");
    return false;
  }
  if (!client_->connect(clientId)) {
    Logger::warn("AWS IoT MQTT connection failed");
    return false;
  }
  return true;
}
void Esp32AwsIotTransport::disconnect() {
  client_->disconnect();
  callback_ = nullptr;
}
bool Esp32AwsIotTransport::isConnected() const { return client_->connected(); }
bool Esp32AwsIotTransport::publish(const std::string &topic,
                                   const std::string &payload, MqttQos qos,
                                   bool retain) {
  return isConnected() && client_->publish(topic, payload, qos, retain);
}
bool Esp32AwsIotTransport::subscribe(const std::string &topic, MqttQos qos,
                                     MqttMessageCallback callback) {
  if (!isConnected() || !client_->subscribe(topic, qos))
    return false;
  callback_ = std::move(callback);
  return true;
}
void Esp32AwsIotTransport::poll() {
  if (isConnected())
    client_->poll();
}

FakeDeviceTransport::FakeDeviceTransport()
    : connected_(false), connectFailuresRemaining_(0),
      publishFailuresRemaining_(0), subscribeFailuresRemaining_(0),
      publishCallCount_(0), failPublishCall_(0) {}
bool FakeDeviceTransport::connect(const std::string &id) {
  if (connectFailuresRemaining_ > 0) {
    --connectFailuresRemaining_;
    return connected_ = false;
  }
  clientId_ = id;
  return connected_ = true;
}
void FakeDeviceTransport::disconnect() {
  connected_ = false;
  subscriptions_.clear();
}
bool FakeDeviceTransport::isConnected() const { return connected_; }
bool FakeDeviceTransport::publish(const std::string &t, const std::string &p,
                                  MqttQos q, bool r) {
  if (!connected_)
    return false;
  ++publishCallCount_;
  if (publishCallCount_ == failPublishCall_)
    return false;
  if (publishFailuresRemaining_ > 0) {
    --publishFailuresRemaining_;
    return false;
  }
  published_.push_back({t, p, q, r});
  return true;
}
bool FakeDeviceTransport::subscribe(const std::string &t, MqttQos q,
                                    MqttMessageCallback cb) {
  if (!connected_)
    return false;
  if (subscribeFailuresRemaining_ > 0) {
    --subscribeFailuresRemaining_;
    return false;
  }
  subscriptions_.push_back({t, q, std::move(cb)});
  return true;
}
void FakeDeviceTransport::poll() {}
void FakeDeviceTransport::armConnectFailure(int n) {
  connectFailuresRemaining_ = n;
}
void FakeDeviceTransport::armPublishFailure(int n) {
  publishFailuresRemaining_ = n;
}
void FakeDeviceTransport::armPublishFailureOnCall(int n) {
  failPublishCall_ = n;
}
void FakeDeviceTransport::armSubscribeFailure(int n) {
  subscribeFailuresRemaining_ = n;
}
const std::vector<FakeDeviceTransport::PublishedMessage> &
FakeDeviceTransport::publishedMessages() const {
  return published_;
}
const std::vector<FakeDeviceTransport::Subscription> &
FakeDeviceTransport::subscriptions() const {
  return subscriptions_;
}
size_t FakeDeviceTransport::subscriptionCount() const {
  return subscriptions_.size();
}
const std::string &FakeDeviceTransport::lastClientId() const {
  return clientId_;
}
void FakeDeviceTransport::deliver(const std::string &t, const std::string &p) {
  for (const auto &s : subscriptions_)
    if (s.topic == t && s.callback)
      s.callback(t, p);
}
} // namespace interbridge
