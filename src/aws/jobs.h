#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../ota/ota_manager.h"

namespace interbridge {

// A pending AWS IoT Job's execution document. Modeled generically since
// the exact job document schema is an AWS-infrastructure decision not
// finalized yet (template name, presigned-URL issuance mechanism) - see
// docs/communication-protocol.md > AWS IoT Jobs / OTA Manager.
struct JobDocument {
    std::string jobId;
    std::string version;
    std::string url; // presigned HTTPS URL, short-lived
    std::string sha256;
};

enum class JobStatus { InProgress, Succeeded, Failed, Rejected };
const char* toString(JobStatus status);

// Low-level AWS IoT Jobs transport: check pending jobs, report status.
// Kept separate from IDeviceTransport because Jobs uses AWS's own
// reserved topics/semantics (see network/mqtt_topics.h), not the generic
// pub/sub used for custom protocol messages. OTA is implemented via AWS
// Jobs, not a custom UPDATE_FIRMWARE application command.
class IJobsClient {
public:
    virtual ~IJobsClient() = default;

    virtual std::optional<JobDocument> checkPendingJob() = 0;
    virtual void updateJobStatus(const std::string& jobId, JobStatus status, const std::string& statusDetails) = 0;
};

// Real ESP32/AWS implementation. STUB: Jobs requires a working MQTT/TLS
// connection (see network/mqtt_transport.h, also a stub) and AWS
// infrastructure that does not exist yet. See CONTEXT.md > Open
// Questions.
class Esp32JobsClient : public IJobsClient {
public:
    std::optional<JobDocument> checkPendingJob() override;
    void updateJobStatus(const std::string& jobId, JobStatus status, const std::string& statusDetails) override;
};

// In-memory fake for native tests: lets a test queue a job document and
// records every status update.
class FakeJobsClient : public IJobsClient {
public:
    struct StatusUpdate {
        std::string jobId;
        JobStatus status;
        std::string statusDetails;
    };

    void queueJob(const JobDocument& job);

    std::optional<JobDocument> checkPendingJob() override;
    void updateJobStatus(const std::string& jobId, JobStatus status, const std::string& statusDetails) override;

    const std::vector<StatusUpdate>& statusUpdates() const;

private:
    std::optional<JobDocument> pendingJob_;
    std::vector<StatusUpdate> statusUpdates_;
};

// Bridges AWS IoT Jobs to the OTA coordinator: checks for a pending job,
// reports IN_PROGRESS, runs it through OtaCoordinator, and reports the
// terminal Jobs status. Does not publish OTA_STARTED/OTA_COMPLETED/
// OTA_FAILED protocol events itself - see device_transport.* for that
// wiring, keeping this class free of a direct MQTT dependency.
class JobsCoordinator {
public:
    JobsCoordinator(IJobsClient& jobsClient, OtaCoordinator& otaCoordinator);

    // Call periodically. Returns the OTA result if a job was found and
    // processed this call, or std::nullopt if there was no pending job.
    std::optional<OtaResult> pollAndProcess();

private:
    IJobsClient& jobsClient_;
    OtaCoordinator& otaCoordinator_;
};

} // namespace interbridge
