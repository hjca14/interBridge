#pragma once

#include <string>

namespace interbridge {

constexpr const char* kDevIngestRuleName = "interbridge_dev_ingest_rule";
constexpr const char* kDevResponseRuleName = "interbridge_dev_response_rule";

// Rule names are explicit environment-owned inputs; there are no
// provisional or production defaults.
struct MqttTopicsConfig {
    std::string deviceId;
    std::string ingestRuleName;
    std::string responseRuleName;
    std::string shadowName = "interbridge";
    std::string fleetProvisioningTemplateName; // empty: not decided yet, see CONTEXT.md
};

MqttTopicsConfig devMqttTopicsConfig(const std::string& deviceId);

// Central builder for every MQTT topic InterBridge uses: the normal
// command-subscription topic, AWS IoT Basic Ingest topics (events,
// health, responses), the named Device Shadow topics, AWS IoT Jobs
// topics, and AWS IoT Fleet Provisioning topics. No other module should
// construct these strings independently - see
// docs/communication-protocol.md > MQTT Topic Builder.
class MqttTopics {
public:
    explicit MqttTopics(MqttTopicsConfig config);

    // Per docs/communication-protocol.md > AWS IoT Policies: "ClientId ==
    // ThingName == device_id" - the MQTT client ID is the device_id
    // itself, not a decorated string.
    static std::string clientId(const std::string& deviceId);

    // Normal broker: the device subscribes here to receive commands.
    std::string commands() const;

    // AWS IoT Basic Ingest (device -> cloud, rule-routed).
    std::string eventsIngest() const;
    std::string healthIngest() const;
    std::string responsesIngest() const;

    // AWS IoT Device Shadow (named shadow, see shadowName in config).
    std::string shadowUpdate() const;
    std::string shadowUpdateAccepted() const;
    std::string shadowUpdateRejected() const;
    std::string shadowUpdateDelta() const;
    std::string shadowGet() const;
    std::string shadowGetAccepted() const;
    std::string shadowGetRejected() const;

    // AWS IoT Jobs (reserved topics, thing_name == device_id).
    std::string jobsNotifyNext() const;
    std::string jobsNextGet() const;
    std::string jobsNextGetAccepted() const;
    std::string jobsNextGetRejected() const;
    std::string jobsUpdate(const std::string& jobId) const;
    std::string jobsUpdateAccepted(const std::string& jobId) const;
    std::string jobsUpdateRejected(const std::string& jobId) const;

    // AWS IoT Fleet Provisioning by trusted user (CSR flow). These use a
    // temporary claim credential, not the permanent device identity.
    std::string fleetProvisioningCreateCertFromCsr() const;
    std::string fleetProvisioningCreateCertFromCsrAccepted() const;
    std::string fleetProvisioningCreateCertFromCsrRejected() const;
    std::string fleetProvisioningRegisterThing() const; // uses fleetProvisioningTemplateName
    std::string fleetProvisioningRegisterThingAccepted() const;
    std::string fleetProvisioningRegisterThingRejected() const;

private:
    std::string shadowTopic(const char* suffix) const;
    std::string jobsTopic(const std::string& suffix) const;

    MqttTopicsConfig config_;
};

} // namespace interbridge
