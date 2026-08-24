#include <unity.h>

#include "dev/si3050_clock_probe_generator_config.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_configured_ratio_matches_requested_geometry() {
    // The geometry the generator actually requests (16 TDM channels x
    // 16 bits/sample) - this is what is REQUESTED, not what a real
    // bench test measured (~64, see docs/si3050-clock-probe.md).
    TEST_ASSERT_EQUAL_UINT32(256, configuredTdmRatio(16, 16));
}

void test_configured_bclk_matches_requested_geometry() {
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
    RUN_TEST(test_configured_ratio_is_pure_multiplication);
    RUN_TEST(test_configured_bclk_is_pure_multiplication);
    return UNITY_END();
}
