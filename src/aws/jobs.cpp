#include "jobs.h"

namespace interbridge {

const char* toString(JobStatus status) {
    switch (status) {
        case JobStatus::InProgress: return "IN_PROGRESS";
        case JobStatus::Succeeded: return "SUCCEEDED";
        case JobStatus::Failed: return "FAILED";
        case JobStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN_JOB_STATUS";
}

std::optional<JobDocument> Esp32JobsClient::checkPendingJob() {
    // TODO: not implemented - requires a working MQTT/TLS connection and
    // AWS IoT Jobs topics. See CONTEXT.md > Open Questions.
    return std::nullopt;
}

void Esp32JobsClient::updateJobStatus(const std::string& jobId, JobStatus status, const std::string& statusDetails) {
    (void)jobId;
    (void)status;
    (void)statusDetails;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
}

void FakeJobsClient::queueJob(const JobDocument& job) {
    pendingJob_ = job;
}

std::optional<JobDocument> FakeJobsClient::checkPendingJob() {
    auto job = pendingJob_;
    pendingJob_.reset();
    return job;
}

void FakeJobsClient::updateJobStatus(const std::string& jobId, JobStatus status, const std::string& statusDetails) {
    statusUpdates_.push_back(StatusUpdate{jobId, status, statusDetails});
}

const std::vector<FakeJobsClient::StatusUpdate>& FakeJobsClient::statusUpdates() const {
    return statusUpdates_;
}

JobsCoordinator::JobsCoordinator(IJobsClient& jobsClient, OtaCoordinator& otaCoordinator)
    : jobsClient_(jobsClient), otaCoordinator_(otaCoordinator) {}

std::optional<OtaResult> JobsCoordinator::pollAndProcess() {
    auto job = jobsClient_.checkPendingJob();
    if (!job.has_value()) {
        return std::nullopt;
    }

    jobsClient_.updateJobStatus(job->jobId, JobStatus::InProgress, "");

    OtaResult result = otaCoordinator_.apply(job->version, job->url, job->sha256);

    if (result == OtaResult::Success) {
        jobsClient_.updateJobStatus(job->jobId, JobStatus::Succeeded, "");
    } else {
        jobsClient_.updateJobStatus(job->jobId, JobStatus::Failed, toString(result));
    }

    return result;
}

} // namespace interbridge
