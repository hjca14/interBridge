#include <unity.h>

#include "intercom/si3050/si3050_pcm_clock.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

// ---- TDM geometry math (physically validated - see
// docs/si3050-clock-probe.md's "Real bench observation: 16 x 8 slot
// geometry reaches the PCM/SPI target") ----

void test_slot_count_and_width_match_the_validated_geometry() {
    TEST_ASSERT_EQUAL_UINT32(16, kSi3050PcmTdmSlotCount);
    TEST_ASSERT_EQUAL_UINT32(8, kSi3050PcmTdmSlotWidthBits);
}

void test_clocks_per_frame_is_16_times_8() {
    TEST_ASSERT_EQUAL_UINT32(128, si3050PcmClocksPerFrame(kSi3050PcmTdmSlotCount, kSi3050PcmTdmSlotWidthBits));
}

void test_requested_pclk_hz_is_8000_times_128() {
    TEST_ASSERT_EQUAL_UINT32(
        1024000, si3050PcmRequestedPclkHz(8000, kSi3050PcmTdmSlotCount, kSi3050PcmTdmSlotWidthBits));
}

// ---- Configuration gate (fail-closed: never silently substitute a
// different pclkHz than the caller asked for) ----

void test_configuration_supported_for_the_validated_target() {
    TEST_ASSERT_TRUE(si3050PcmConfigurationSupported(1024000, 8000));
}

void test_configuration_rejected_for_mismatched_pclk() {
    // The old (pre-validation) GCI-style target - not what this fixed
    // 16 x 8 geometry produces for an 8 kHz FSYNC.
    TEST_ASSERT_FALSE(si3050PcmConfigurationSupported(2048000, 8000));
}

void test_configuration_rejected_for_zero_fsync() {
    // Would otherwise divide-by-zero-adjacent (0 * anything = 0, so a
    // pclkHz of 0 would wrongly "match") - explicitly rejected instead.
    TEST_ASSERT_FALSE(si3050PcmConfigurationSupported(0, 0));
}

void test_configuration_rejected_for_unrelated_pclk_at_a_different_fsync() {
    // 4 kHz FSYNC would need pclkHz=512000 for this geometry, not 1024000.
    TEST_ASSERT_FALSE(si3050PcmConfigurationSupported(1024000, 4000));
}

// ---- Si3050PcmClockBringup: pure bring-up/rollback/idempotency logic
// (mirrors PcntBringupTracker's role for the clock probe meter - see
// src/dev/si3050_clock_probe_meter_bringup.h - but this class has no
// dependency on that dev-only module). The real ESP-IDF calls
// themselves are not exercised here - only the pure decision/tracking
// logic around them, same limitation as that tracker. ----

void test_bringup_should_start_initially() {
    Si3050PcmClockBringup bringup;
    TEST_ASSERT_TRUE(bringup.shouldStart());
    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.hasFailed());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
}

void test_bringup_succeeds_when_every_step_succeeds() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    TEST_ASSERT_TRUE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.hasFailed());
    TEST_ASSERT_TRUE(bringup.shouldUninstall()); // driver held while running
    TEST_ASSERT_FALSE(bringup.shouldStart());    // idempotent: a second start() must no-op
}

void test_bringup_fails_on_driver_install_with_no_rollback_owed() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(-1, false);

    TEST_ASSERT_TRUE(bringup.hasFailed());
    TEST_ASSERT_EQUAL_STRING("i2s_driver_install", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-1, bringup.failedStepResult());
    TEST_ASSERT_FALSE(bringup.isRunning());
    // Nothing was actually installed - stop()/rollback must not call
    // i2s_driver_uninstall() for a step that never succeeded.
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
}

void test_bringup_fails_on_pin_config_with_rollback_owed() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", -2, false);

    TEST_ASSERT_TRUE(bringup.hasFailed());
    TEST_ASSERT_EQUAL_STRING("i2s_set_pin", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-2, bringup.failedStepResult());
    TEST_ASSERT_FALSE(bringup.isRunning());
    // The driver WAS installed before this step failed - rollback
    // (i2s_driver_uninstall()) is owed.
    TEST_ASSERT_TRUE(bringup.shouldUninstall());
}

void test_bringup_fails_on_zero_dma_buffer_with_rollback_owed() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", -3, false);

    TEST_ASSERT_TRUE(bringup.hasFailed());
    TEST_ASSERT_EQUAL_STRING("i2s_zero_dma_buffer", bringup.failedStepName());
    TEST_ASSERT_TRUE(bringup.shouldUninstall());
}

void test_bringup_records_only_the_first_failure() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", -2, false);       // root cause
    bringup.record("i2s_zero_dma_buffer", -3, false); // must not overwrite it

    TEST_ASSERT_EQUAL_STRING("i2s_set_pin", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-2, bringup.failedStepResult());
}

void test_bringup_rollback_resets_state_for_a_clean_retry() {
    Si3050PcmClockBringup bringup;

    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", -2, false);
    TEST_ASSERT_TRUE(bringup.shouldUninstall());

    bringup.recordUninstall(kSi3050PcmClockEspOk, true); // caller's real i2s_driver_uninstall() succeeded

    TEST_ASSERT_FALSE(bringup.shouldUninstall());
    TEST_ASSERT_FALSE(bringup.hasFailed());
    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_TRUE(bringup.shouldStart()); // a fresh start() attempt is now allowed
}

void test_bringup_stop_when_never_started_is_a_safe_no_op() {
    Si3050PcmClockBringup bringup;

    // Esp32PcmClock::stop() only calls recordUninstall() when
    // shouldUninstall() is true - here it is not, so this models "stop()
    // was a no-op" rather than a real call; state must already be safe.

    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.hasFailed());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
    TEST_ASSERT_TRUE(bringup.shouldStart());
}

void test_bringup_stop_after_running_then_repeated_stop_is_safe() {
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    bringup.recordUninstall(kSi3050PcmClockEspOk, true); // first stop(), real uninstall succeeds
    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());

    // Second stop(): shouldUninstall() is now false, so Esp32PcmClock::
    // stop() would not call recordUninstall() again at all - state stays
    // exactly as it was, safely.
    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
}

void test_bringup_mark_running_is_a_no_op_after_a_failure() {
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(-1, false);

    bringup.markRunning(); // must not override a failed attempt

    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_TRUE(bringup.hasFailed());
}

// ---- Regression: a transient i2s_driver_install() failure must not
// permanently block retry. Before this fix, recordDriverInstall() left
// hasFailed_ set forever once the install step itself failed (nothing
// was ever acquired, so nothing ever called the reset), and every
// following record()/markRunning() call silently no-op'd because of the
// "if (hasFailed_) return;" guard - shouldStart() kept returning true
// (it only checked running_), so start() looked idempotent-safe to
// retry, but every retry's outcome was discarded, wedging the clock
// permanently "not running" until reboot. ----

void test_bringup_install_failure_allows_a_full_retry_that_succeeds() {
    Si3050PcmClockBringup bringup;

    // First logical start(): i2s_driver_install() itself fails.
    TEST_ASSERT_TRUE(bringup.shouldStart());
    bringup.recordDriverInstall(-1, false);
    TEST_ASSERT_TRUE(bringup.hasFailed());
    TEST_ASSERT_EQUAL_STRING("i2s_driver_install", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-1, bringup.failedStepResult());
    // Nothing was acquired - no real i2s_driver_uninstall() call is owed.
    TEST_ASSERT_FALSE(bringup.shouldUninstall());

    // The caller has now logged the original failure (failedStepName()/
    // failedStepResult() above, read before this call) - clean up the
    // attempt so a later start() is not stuck on it.
    bringup.resetAfterInstallFailure();
    TEST_ASSERT_FALSE(bringup.hasFailed());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
    TEST_ASSERT_TRUE(bringup.shouldStart()); // must be retryable, not wedged forever

    // Second logical start(): install and every later step succeed.
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    TEST_ASSERT_TRUE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.hasFailed());
}

// ---- i2s_driver_uninstall()'s own return value must never be assumed
// ESP_OK: a failed uninstall means the driver may still be active, so
// the resource must stay considered held (shouldUninstall() keeps
// reporting true) and no new install may be attempted on top of it
// (shouldStart() becomes false) until a retry actually reports success. ----

void test_bringup_uninstall_success_confirms_release_and_allows_restart() {
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    bringup.recordUninstall(kSi3050PcmClockEspOk, true); // real i2s_driver_uninstall() succeeded

    TEST_ASSERT_FALSE(bringup.isRunning());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
    TEST_ASSERT_TRUE(bringup.shouldStart());
    TEST_ASSERT_FALSE(bringup.hasFailed());
}

void test_bringup_uninstall_failure_stays_fail_closed_and_does_not_claim_release() {
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    bringup.recordUninstall(-9, false); // real i2s_driver_uninstall() itself failed

    // Must never be treated as safely released.
    TEST_ASSERT_TRUE(bringup.shouldUninstall());  // a retry is still owed/worth attempting
    TEST_ASSERT_FALSE(bringup.shouldStart());     // fail-closed: never install a second instance
                                                   // on top of a possibly-still-active driver
    TEST_ASSERT_TRUE(bringup.hasFailed());
    TEST_ASSERT_EQUAL_STRING("i2s_driver_uninstall", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-9, bringup.failedStepResult());
}

void test_bringup_uninstall_retry_after_failure_can_eventually_succeed() {
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", kSi3050PcmClockEspOk, true);
    bringup.record("i2s_zero_dma_buffer", kSi3050PcmClockEspOk, true);
    bringup.markRunning();

    bringup.recordUninstall(-9, false); // first teardown attempt fails
    TEST_ASSERT_FALSE(bringup.shouldStart());

    bringup.recordUninstall(kSi3050PcmClockEspOk, true); // a later retry succeeds
    TEST_ASSERT_TRUE(bringup.shouldStart());
    TEST_ASSERT_FALSE(bringup.shouldUninstall());
    TEST_ASSERT_FALSE(bringup.hasFailed());
}

void test_bringup_rollback_after_bringup_failure_reports_uninstall_failure() {
    // A bring-up failure (pin config) followed by a rollback whose OWN
    // i2s_driver_uninstall() call also fails - must not be silently
    // treated as released either.
    Si3050PcmClockBringup bringup;
    bringup.recordDriverInstall(kSi3050PcmClockEspOk, true);
    bringup.record("i2s_set_pin", -2, false);
    TEST_ASSERT_TRUE(bringup.shouldUninstall());

    bringup.recordUninstall(-7, false); // the rollback's own uninstall call fails

    TEST_ASSERT_TRUE(bringup.shouldUninstall());
    TEST_ASSERT_FALSE(bringup.shouldStart());
    TEST_ASSERT_EQUAL_STRING("i2s_driver_uninstall", bringup.failedStepName());
    TEST_ASSERT_EQUAL(-7, bringup.failedStepResult());
}

// ---- Esp32PcmClock on the native (host) build: no real I2S peripheral
// exists here, matching every other real (Esp32*) Si3050 collaborator's
// native-build behavior in this module (e.g. Esp32Si3050Bus::transfer()
// returning a fixed value on native) - only the safe-no-hardware-touched
// behavior is asserted, not real bring-up (see the Si3050PcmClockBringup
// tests above for that). ----

void test_esp32_pcm_clock_starts_not_running() {
    Esp32PcmClock clock;
    TEST_ASSERT_FALSE(clock.isRunning());
}

void test_esp32_pcm_clock_stop_without_start_is_safe() {
    Esp32PcmClock clock;
    clock.stop();
    TEST_ASSERT_FALSE(clock.isRunning());
}

void test_esp32_pcm_clock_repeated_stop_is_safe() {
    Esp32PcmClock clock;
    clock.stop();
    clock.stop();
    TEST_ASSERT_FALSE(clock.isRunning());
}

void test_esp32_pcm_clock_start_on_host_stays_not_running() {
    // No real I2S peripheral on the native host - isRunning() must stay
    // false rather than ever claim success without real hardware.
    Esp32PcmClock clock;
    clock.start(1024000, 8000);
    TEST_ASSERT_FALSE(clock.isRunning());
}

void test_esp32_pcm_clock_rejects_unsupported_config_without_crashing() {
    Esp32PcmClock clock;
    clock.start(2048000, 8000); // the old, unsupported GCI-style target
    TEST_ASSERT_FALSE(clock.isRunning());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_count_and_width_match_the_validated_geometry);
    RUN_TEST(test_clocks_per_frame_is_16_times_8);
    RUN_TEST(test_requested_pclk_hz_is_8000_times_128);
    RUN_TEST(test_configuration_supported_for_the_validated_target);
    RUN_TEST(test_configuration_rejected_for_mismatched_pclk);
    RUN_TEST(test_configuration_rejected_for_zero_fsync);
    RUN_TEST(test_configuration_rejected_for_unrelated_pclk_at_a_different_fsync);
    RUN_TEST(test_bringup_should_start_initially);
    RUN_TEST(test_bringup_succeeds_when_every_step_succeeds);
    RUN_TEST(test_bringup_fails_on_driver_install_with_no_rollback_owed);
    RUN_TEST(test_bringup_fails_on_pin_config_with_rollback_owed);
    RUN_TEST(test_bringup_fails_on_zero_dma_buffer_with_rollback_owed);
    RUN_TEST(test_bringup_records_only_the_first_failure);
    RUN_TEST(test_bringup_rollback_resets_state_for_a_clean_retry);
    RUN_TEST(test_bringup_stop_when_never_started_is_a_safe_no_op);
    RUN_TEST(test_bringup_stop_after_running_then_repeated_stop_is_safe);
    RUN_TEST(test_bringup_mark_running_is_a_no_op_after_a_failure);
    RUN_TEST(test_bringup_install_failure_allows_a_full_retry_that_succeeds);
    RUN_TEST(test_bringup_uninstall_success_confirms_release_and_allows_restart);
    RUN_TEST(test_bringup_uninstall_failure_stays_fail_closed_and_does_not_claim_release);
    RUN_TEST(test_bringup_uninstall_retry_after_failure_can_eventually_succeed);
    RUN_TEST(test_bringup_rollback_after_bringup_failure_reports_uninstall_failure);
    RUN_TEST(test_esp32_pcm_clock_starts_not_running);
    RUN_TEST(test_esp32_pcm_clock_stop_without_start_is_safe);
    RUN_TEST(test_esp32_pcm_clock_repeated_stop_is_safe);
    RUN_TEST(test_esp32_pcm_clock_start_on_host_stays_not_running);
    RUN_TEST(test_esp32_pcm_clock_rejects_unsupported_config_without_crashing);
    return UNITY_END();
}
