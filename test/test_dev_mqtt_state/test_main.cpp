#include <unity.h>

#include "dev/mqtt_smoke_state.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_happy_path_and_time_gate() {
    DevMqttSmokeState state(10, 40);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectWifi), static_cast<int>(state.update(0, false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(1, true, false, false)));
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime), static_cast<int>(state.update(1, true, false, false)));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt),
                          static_cast<int>(state.update(10, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(11, true, true, false)));
    state.mqttResult(11, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

void test_dns_and_mqtt_failures_back_off_and_recover() {
    DevMqttSmokeState state(10, 40);
    state.update(0, false, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(1, true, false, false)));
    state.dnsResult(1, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(10, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(11, true, false, false)));
    state.dnsResult(11, true);
    state.update(11, true, false, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(21, true, true, false)));
    state.mqttResult(21, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None), static_cast<int>(state.update(30, true, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConnectMqtt), static_cast<int>(state.update(31, true, true, false)));
    state.mqttResult(31, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

void test_wifi_loss_requires_all_gates_again() {
    DevMqttSmokeState state(10, 40);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, false, false);
    state.update(11, true, true, false);
    state.mqttResult(11, true);
    state.update(12, false, true, false);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::WaitingForWifi), static_cast<int>(state.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ResolveDns), static_cast<int>(state.update(13, true, true, false)));
}

void test_backoff_is_capped_and_deadline_wrap_is_safe() {
    DevMqttSmokeState state(10, 20);
    state.update(0, false, false, false);
    state.update(10, false, false, false);
    state.update(30, false, false, false);
    TEST_ASSERT_EQUAL_UINT32(20, state.retryDelayMs());
    TEST_ASSERT_FALSE(DevMqttSmokeState::deadlineReached(0xfffffff0u, 0x00000005u));
    TEST_ASSERT_TRUE(DevMqttSmokeState::deadlineReached(0x00000006u, 0x00000005u));
}

void test_time_sync_in_progress_defers_reissuing_configure_time() {
    DevMqttSmokeState state(10, 40);
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(1, true, false, false)));
    // The backoff deadline (t=11) is reached, but a previous SNTP attempt is
    // still reported in progress - must not restart it.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(11, true, false, false, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::None),
                      static_cast<int>(state.update(15, true, false, false, true)));
    // Once no longer in progress and still not time-valid, the already-
    // elapsed deadline fires immediately - no extra wait was imposed just
    // because it was deferred.
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeAction::ConfigureTime),
                      static_cast<int>(state.update(16, true, false, false, false)));
}

void test_observation_only_update_does_not_change_online_state() {
    DevMqttSmokeState state;
    state.update(0, false, false, false);
    state.update(1, true, false, false);
    state.dnsResult(1, true);
    state.update(1, true, true, false);
    state.mqttResult(1, true);
    state.update(50000, true, true, true);
    TEST_ASSERT_EQUAL(static_cast<int>(DevSmokeState::Online), static_cast<int>(state.state()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_and_time_gate);
    RUN_TEST(test_dns_and_mqtt_failures_back_off_and_recover);
    RUN_TEST(test_wifi_loss_requires_all_gates_again);
    RUN_TEST(test_backoff_is_capped_and_deadline_wrap_is_safe);
    RUN_TEST(test_time_sync_in_progress_defers_reissuing_configure_time);
    RUN_TEST(test_observation_only_update_does_not_change_online_state);
    return UNITY_END();
}
