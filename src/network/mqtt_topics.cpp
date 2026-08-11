#include "mqtt_topics.h"

namespace interbridge {

MqttTopics::MqttTopics(MqttTopicsConfig config) : config_(std::move(config)) {}

std::string MqttTopics::clientId(const std::string& deviceId) {
    return deviceId;
}

std::string MqttTopics::commands() const {
    return "interbridge/" + config_.deviceId + "/commands";
}

std::string MqttTopics::eventsIngest() const {
    return "$aws/rules/" + config_.ingestRuleName + "/interbridge/" + config_.deviceId + "/events";
}

std::string MqttTopics::healthIngest() const {
    return "$aws/rules/" + config_.ingestRuleName + "/interbridge/" + config_.deviceId + "/health";
}

std::string MqttTopics::responsesIngest() const {
    return "$aws/rules/" + config_.responseRuleName + "/interbridge/" + config_.deviceId + "/responses";
}

std::string MqttTopics::shadowTopic(const char* suffix) const {
    return "$aws/things/" + config_.deviceId + "/shadow/name/" + config_.shadowName + "/" + suffix;
}

std::string MqttTopics::shadowUpdate() const {
    return shadowTopic("update");
}

std::string MqttTopics::shadowUpdateAccepted() const {
    return shadowTopic("update/accepted");
}

std::string MqttTopics::shadowUpdateRejected() const {
    return shadowTopic("update/rejected");
}

std::string MqttTopics::shadowUpdateDelta() const {
    return shadowTopic("update/delta");
}

std::string MqttTopics::shadowGet() const {
    return shadowTopic("get");
}

std::string MqttTopics::shadowGetAccepted() const {
    return shadowTopic("get/accepted");
}

std::string MqttTopics::shadowGetRejected() const {
    return shadowTopic("get/rejected");
}

std::string MqttTopics::jobsTopic(const std::string& suffix) const {
    return "$aws/things/" + config_.deviceId + "/jobs/" + suffix;
}

std::string MqttTopics::jobsNotifyNext() const {
    return jobsTopic("notify-next");
}

std::string MqttTopics::jobsNextGet() const {
    return jobsTopic("$next/get");
}

std::string MqttTopics::jobsNextGetAccepted() const {
    return jobsTopic("$next/get/accepted");
}

std::string MqttTopics::jobsNextGetRejected() const {
    return jobsTopic("$next/get/rejected");
}

std::string MqttTopics::jobsUpdate(const std::string& jobId) const {
    return jobsTopic(jobId + "/update");
}

std::string MqttTopics::jobsUpdateAccepted(const std::string& jobId) const {
    return jobsTopic(jobId + "/update/accepted");
}

std::string MqttTopics::jobsUpdateRejected(const std::string& jobId) const {
    return jobsTopic(jobId + "/update/rejected");
}

std::string MqttTopics::fleetProvisioningCreateCertFromCsr() const {
    return "$aws/certificates/create-from-csr/json";
}

std::string MqttTopics::fleetProvisioningCreateCertFromCsrAccepted() const {
    return "$aws/certificates/create-from-csr/json/accepted";
}

std::string MqttTopics::fleetProvisioningCreateCertFromCsrRejected() const {
    return "$aws/certificates/create-from-csr/json/rejected";
}

std::string MqttTopics::fleetProvisioningRegisterThing() const {
    if (config_.fleetProvisioningTemplateName.empty()) {
        return "";
    }
    return "$aws/provisioning-templates/" + config_.fleetProvisioningTemplateName + "/provision/json";
}

std::string MqttTopics::fleetProvisioningRegisterThingAccepted() const {
    if (config_.fleetProvisioningTemplateName.empty()) {
        return "";
    }
    return "$aws/provisioning-templates/" + config_.fleetProvisioningTemplateName + "/provision/json/accepted";
}

std::string MqttTopics::fleetProvisioningRegisterThingRejected() const {
    if (config_.fleetProvisioningTemplateName.empty()) {
        return "";
    }
    return "$aws/provisioning-templates/" + config_.fleetProvisioningTemplateName + "/provision/json/rejected";
}

} // namespace interbridge
