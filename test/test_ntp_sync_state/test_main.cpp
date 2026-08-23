#include <unity.h>
#include "hardware/ntp_sync_state.h"

using namespace interbridge;
void setUp() {}
void tearDown() {}

void test_epoch_like_value_does_not_replace_completed_sync() {
    NtpSyncState state(1000);
    TEST_ASSERT_FALSE(state.isTrustworthy(2000, false));
}
void test_transition_requires_completion_settle_and_no_sync_in_progress() {
    NtpSyncState state(1000);
    state.synchronizationStarted();
    TEST_ASSERT_FALSE(state.isTrustworthy(5000, false));
    state.synchronizationCompleted(5000);
    TEST_ASSERT_FALSE(state.isTrustworthy(5999, false));
    TEST_ASSERT_FALSE(state.isTrustworthy(6000, true));
    TEST_ASSERT_TRUE(state.isTrustworthy(6000, false));
}
void test_settle_deadline_is_millis_wrap_safe() {
    NtpSyncState state(32);
    state.synchronizationCompleted(0xfffffff0u);
    TEST_ASSERT_FALSE(state.isTrustworthy(0x0000000fu, false));
    TEST_ASSERT_TRUE(state.isTrustworthy(0x00000010u, false));
}
int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_epoch_like_value_does_not_replace_completed_sync);
    RUN_TEST(test_transition_requires_completion_settle_and_no_sync_in_progress);
    RUN_TEST(test_settle_deadline_is_millis_wrap_safe);
    return UNITY_END();
}
