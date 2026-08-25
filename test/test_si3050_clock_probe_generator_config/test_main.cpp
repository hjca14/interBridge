#include <unity.h>

#include "dev/si3050_clock_probe_generator_config.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_configured_ratio_matches_requested_geometry() {
    // The geometry the generator actually requests (16 TDM channels x 8
    // bits/sample = 16 timeslots x 8 bits, matching the Si3050's
    // PCM/SPI-mode PCM Highway geometry per docs/si3050-clock-probe.md's
    // "Corrected premise") = 128 PCLK cycles/frame. This is what is
    // REQUESTED - a real bench retest of this exact geometry confirmed
    // it physically (pclk_hz ~= 1,024,100, ratio ~= 127.98-128.00 across
    // stable windows) - see docs/si3050-clock-probe.md's "Real bench
    // observation: 16 x 8 slot geometry reaches the PCM/SPI target".
    TEST_ASSERT_EQUAL_UINT32(128, configuredTdmRatio(16, 8));
}

void test_configured_bclk_matches_requested_geometry() {
    TEST_ASSERT_EQUAL_UINT32(1024000, configuredBclkHz(8000, 16, 8));
}

void test_configured_ratio_matches_previous_geometry() {
    // The generator's PREVIOUS geometry (16 TDM channels x 16
    // bits/sample, before it was changed to 16 x 8) - kept as a
    // regression check on the pure math, not because the generator still
    // requests this. A real bench test of this geometry measured an
    // actual ratio of ~64, not 256 - see docs/si3050-clock-probe.md's
    // "Real bench observation: generator does not reach the target
    // ratio".
    TEST_ASSERT_EQUAL_UINT32(256, configuredTdmRatio(16, 16));
}

void test_configured_bclk_matches_previous_geometry() {
    TEST_ASSERT_EQUAL_UINT32(2048000, configuredBclkHz(8000, 16, 16));
}

void test_configured_ratio_is_pure_multiplication() {
    TEST_ASSERT_EQUAL_UINT32(64, configuredTdmRatio(2, 32));
    TEST_ASSERT_EQUAL_UINT32(0, configuredTdmRatio(0, 16));
}

void test_configured_bclk_is_pure_multiplication() {
    TEST_ASSERT_EQUAL_UINT32(512000, configuredBclkHz(8000, 2, 32));
    TEST_ASSERT_EQUAL_UINT32(0, configuredBclkHz(0, 16, 16));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_configured_ratio_matches_requested_geometry);
    RUN_TEST(test_configured_bclk_matches_requested_geometry);
    RUN_TEST(test_configured_ratio_matches_previous_geometry);
    RUN_TEST(test_configured_bclk_matches_previous_geometry);
    RUN_TEST(test_configured_ratio_is_pure_multiplication);
    RUN_TEST(test_configured_bclk_is_pure_multiplication);
    return UNITY_END();
}
