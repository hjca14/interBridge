#include <unity.h>

#include "../../src/aws/jobs.h"
#include "../../src/ota/firmware_validation.h"
#include "../../src/ota/ota_manager.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_poll_with_no_pending_job_returns_nullopt() {
    FakeJobsClient jobsClient;
    FakeOtaPlatform platform;
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");
    JobsCoordinator coordinator(jobsClient, ota);

    TEST_ASSERT_FALSE(coordinator.pollAndProcess().has_value());
    TEST_ASSERT_EQUAL(0, static_cast<int>(jobsClient.statusUpdates().size()));
}

void test_successful_job_reports_in_progress_then_succeeded() {
    FakeJobsClient jobsClient;
    jobsClient.queueJob(JobDocument{"job-1", "0.2.0", "https://example.invalid/fw.bin", "deadbeef"});

    FakeOtaPlatform platform;
    platform.setDownloadResult(std::string("deadbeef"));
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");
    JobsCoordinator coordinator(jobsClient, ota);

    auto result = coordinator.pollAndProcess();

    TEST_ASSERT_TRUE(result.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(OtaResult::Success), static_cast<int>(*result));

    const auto& updates = jobsClient.statusUpdates();
    TEST_ASSERT_EQUAL(2, static_cast<int>(updates.size()));
    TEST_ASSERT_EQUAL(static_cast<int>(JobStatus::InProgress), static_cast<int>(updates[0].status));
    TEST_ASSERT_EQUAL(static_cast<int>(JobStatus::Succeeded), static_cast<int>(updates[1].status));
}

void test_failed_job_reports_failed_status_with_details() {
    FakeJobsClient jobsClient;
    jobsClient.queueJob(JobDocument{"job-1", "0.2.0", "https://example.invalid/fw.bin", "deadbeef"});

    FakeOtaPlatform platform;
    platform.setDownloadResult(std::nullopt); // download fails
    FakeFirmwareVerifier verifier;
    OtaCoordinator ota(platform, verifier, "0.1.0");
    JobsCoordinator coordinator(jobsClient, ota);

    coordinator.pollAndProcess();

    const auto& updates = jobsClient.statusUpdates();
    TEST_ASSERT_EQUAL(2, static_cast<int>(updates.size()));
    TEST_ASSERT_EQUAL(static_cast<int>(JobStatus::Failed), static_cast<int>(updates[1].status));
    TEST_ASSERT_EQUAL_STRING("DOWNLOAD_FAILED", updates[1].statusDetails.c_str());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_poll_with_no_pending_job_returns_nullopt);
    RUN_TEST(test_successful_job_reports_in_progress_then_succeeded);
    RUN_TEST(test_failed_job_reports_failed_status_with_details);
    return UNITY_END();
}
