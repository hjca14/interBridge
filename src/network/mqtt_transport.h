#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../storage/credential_store.h"

namespace interbridge {

enum class MqttQos { AtMostOnce = 0, AtLeastOnce = 1 };
using MqttMessageCallback =
    std::function<void(const std::string &, const std::string &)>;

struct AwsIotConnectionConfig {
  std::string endpoint;
  std::string region;
  std::string rootCaPem;
  uint16_t port = 8883;
  uint16_t keepAliveSeconds = 30;
  uint16_t timeoutMs = 1500;
};

// Narrow seam around the mature 256dpi/MQTT + WiFiClientSecure stack. Tests
// replace this interface; firmware never implements MQTT or TLS itself.
class IMqttClient {
public:
  virtual ~IMqttClient() = default;
  virtual bool configureTls(const std::string &endpoint, uint16_t port,
                            const std::string &rootCa,
                            const std::string &certificate,
                            const std::string &privateKey,
                            uint16_t keepAliveSeconds, uint16_t timeoutMs) = 0;
  virtual void setMessageCallback(MqttMessageCallback callback) = 0;
  virtual bool connect(const std::string &clientId) = 0;
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;
  virtual bool publish(const std::string &topic, const std::string &payload,
                       MqttQos qos, bool retain) = 0;
  virtual bool subscribe(const std::string &topic, MqttQos qos) = 0;
  virtual void poll() = 0;
};

class IDeviceTransport {
public:
  virtual ~IDeviceTransport() = default;
  virtual bool connect(const std::string &clientId) = 0;
  virtual void disconnect() = 0;
  virtual bool isConnected() const = 0;
  virtual bool publish(const std::string &topic, const std::string &payload,
                       MqttQos qos, bool retain = false) = 0;
  virtual bool subscribe(const std::string &topic, MqttQos qos,
                         MqttMessageCallback callback) = 0;
  virtual void poll() = 0;
};

class Esp32AwsIotTransport : public IDeviceTransport {
public:
  Esp32AwsIotTransport(AwsIotConnectionConfig config,
                       DeviceCredentialStore &credentials);
  Esp32AwsIotTransport(AwsIotConnectionConfig config,
                       DeviceCredentialStore &credentials, IMqttClient &client);
  ~Esp32AwsIotTransport();

  bool connect(const std::string &clientId) override;
  void disconnect() override;
  bool isConnected() const override;
  bool publish(const std::string &topic, const std::string &payload,
               MqttQos qos, bool retain = false) override;
  bool subscribe(const std::string &topic, MqttQos qos,
                 MqttMessageCallback callback) override;
  void poll() override;

  static bool validEndpoint(const std::string &endpoint);
  static bool validDeviceId(const std::string &deviceId);

private:
  AwsIotConnectionConfig config_;
  DeviceCredentialStore &credentials_;
  std::unique_ptr<IMqttClient> ownedClient_;
  IMqttClient *client_;
  MqttMessageCallback callback_;
  // WiFiClientSecure may retain PEM pointers, so credential buffers must
  // outlive the TLS session. They are never exposed to logging/diagnostics.
  std::string certificatePem_;
  std::string privateKeyPem_;
};

class FakeDeviceTransport : public IDeviceTransport {
public:
  struct PublishedMessage {
    std::string topic;
    std::string payload;
    MqttQos qos;
    bool retain;
  };
  struct Subscription {
    std::string topic;
    MqttQos qos;
    MqttMessageCallback callback;
  };
  FakeDeviceTransport();
  bool connect(const std::string &clientId) override;
  void disconnect() override;
  bool isConnected() const override;
  bool publish(const std::string &topic, const std::string &payload,
               MqttQos qos, bool retain = false) override;
  bool subscribe(const std::string &topic, MqttQos qos,
                 MqttMessageCallback callback) override;
  void poll() override;
  void armConnectFailure(int timesToFail);
  void armPublishFailure(int timesToFail);
  void armPublishFailureOnCall(int callNumber);
  void armSubscribeFailure(int timesToFail);
  const std::vector<PublishedMessage> &publishedMessages() const;
  const std::vector<Subscription> &subscriptions() const;
  size_t subscriptionCount() const;
  const std::string &lastClientId() const;
  void deliver(const std::string &topic, const std::string &payload);

private:
  bool connected_;
  int connectFailuresRemaining_, publishFailuresRemaining_,
      subscribeFailuresRemaining_;
  int publishCallCount_, failPublishCall_;
  std::string clientId_;
  std::vector<PublishedMessage> published_;
  std::vector<Subscription> subscriptions_;
};
} // namespace interbridge
