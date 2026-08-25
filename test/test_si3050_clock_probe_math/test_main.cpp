#include <unity.h>

#include <limits>

#include "dev/si3050_clock_probe_math.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_ratio_at_pcm_spi_target_frequencies_is_128() {
    // 1.024 MHz PCLK / 8 kHz FSYNC over the same window = 128 - the
    // Si3050's PCM/SPI-mode target (16 timeslots of 8 bits per frame),
    // which is the mode InterBridge plans to use. This is NOT the same as
    // GCI mode's 2.048/4.096 MHz requirement - see docs/si3050-clock-probe.md.
    TEST_ASSERT_EQUAL_DOUBLE(static_cast<double>(kPcmSpiTargetRatio),
                             pclkToFsyncRatio(kPcmSpiTargetPclkHz, kPcmSpiTargetFsyncHz));
}

void test_window_result_at_pcm_spi_target_frequencies() {
    ClockProbeWindowResult result =
        computeClockProbeWindowResult(1000000, kPcmSpiTargetPclkHz, kPcmSpiTargetFsyncHz);
    TEST_ASSERT_EQUAL_UINT64(1000000, result.windowMicros);
    TEST_ASSERT_EQUAL_UINT64(kPcmSpiTargetPclkHz, result.pclkRisingEdges);
    TEST_ASSERT_EQUAL_UINT64(kPcmSpiTargetFsyncHz, result.fsyncRisingEdges);
    TEST_ASSERT_EQUAL_DOUBLE(1024000.0, result.pclkHz);
    TEST_ASSERT_EQUAL_DOUBLE(8000.0, result.fsyncHz);
    TEST_ASSERT_EQUAL_DOUBLE(128.0, result.ratio);
}

void test_frequency_conversion_uses_real_window_duration() {
    // A half-second window (not the "usual" ~1 s) with half as many
    // edges as a full second at 2.048 MHz must still resolve to the same
    // 2.048 MHz - the math must use the real window, never assume 1 s.
    TEST_ASSERT_EQUAL_DOUBLE(2048000.0, pulseFrequencyHz(1024000, 500000));
}

void test_zero_and_invalid_counts_are_handled_without_dividing_by_zero() {
    TEST_ASSERT_EQUAL_DOUBLE(0.0, pulseFrequencyHz(0, 1000000)); // zero edges is a valid (if unusual) reading
    TEST_ASSERT_EQUAL_DOUBLE(0.0, pulseFrequencyHz(1000, 0));    // zero window - degenerate, must not divide by zero
    TEST_ASSERT_EQUAL_DOUBLE(0.0, pclkToFsyncRatio(1000, 0));    // zero fsync edges - must not divide by zero
    TEST_ASSERT_EQUAL_UINT64(0, combinePulseCount(0, 100, 0));   // hLimit=0 is invalid configuration
    TEST_ASSERT_EQUAL_UINT64(0, combinePulseCount(0, -5, 30000)); // negative raw count is inconsistent
}

void test_overflow_combination_and_out_of_range_raw_count() {
    // Three full overflow cycles plus a partial remainder.
    TEST_ASSERT_EQUAL_UINT64(91234, combinePulseCount(3, 1234, 30000));
    // Raw count exactly at the H_LIM boundary is still valid.
    TEST_ASSERT_EQUAL_UINT64(30000, combinePulseCount(0, 30000, 30000));
    // Raw count beyond hLimit is inconsistent with the configured
    // watchpoint (e.g. a torn read) - treated as invalid, not a huge or
    // silently-wrong total.
    TEST_ASSERT_EQUAL_UINT64(0, combinePulseCount(0, 30001, 30000));
}

void test_min_max_tracker_tracks_extremes_and_ignores_non_finite_samples() {
    ClockProbeMinMaxTracker stats;
    TEST_ASSERT_FALSE(stats.hasSample());

    stats.observe(5.0);
    stats.observe(2.0);
    stats.observe(9.0);
    stats.observe(std::numeric_limits<double>::quiet_NaN());
    stats.observe(std::numeric_limits<double>::infinity());

    TEST_ASSERT_TRUE(stats.hasSample());
    TEST_ASSERT_EQUAL_DOUBLE(2.0, stats.minValue());
    TEST_ASSERT_EQUAL_DOUBLE(9.0, stats.maxValue());
}

void test_min_max_tracker_single_sample_is_both_min_and_max() {
    ClockProbeMinMaxTracker stats;
    stats.observe(42.0);
    TEST_ASSERT_TRUE(stats.hasSample());
    TEST_ASSERT_EQUAL_DOUBLE(42.0, stats.minValue());
    TEST_ASSERT_EQUAL_DOUBLE(42.0, stats.maxValue());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ratio_at_pcm_spi_target_frequencies_is_128);
    RUN_TEST(test_window_result_at_pcm_spi_target_frequencies);
    RUN_TEST(test_frequency_conversion_uses_real_window_duration);
    RUN_TEST(test_zero_and_invalid_counts_are_handled_without_dividing_by_zero);
    RUN_TEST(test_overflow_combination_and_out_of_range_raw_count);
    RUN_TEST(test_min_max_tracker_tracks_extremes_and_ignores_non_finite_samples);
    RUN_TEST(test_min_max_tracker_single_sample_is_both_min_and_max);
    return UNITY_END();
}
