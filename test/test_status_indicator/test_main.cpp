#include <unity.h>

#include "../../src/hardware/status_indicator.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_no_indication_before_first_show() {
    FakeStatusIndicator indicator;
    TEST_ASSERT_FALSE(indicator.hasIndication());
}

void test_show_records_indication() {
    FakeStatusIndicator indicator;
    indicator.show(ProvisioningIndication::ProvisioningAvailable);

    TEST_ASSERT_TRUE(indicator.hasIndication());
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningIndication::ProvisioningAvailable),
                       static_cast<int>(indicator.lastIndication()));
    TEST_ASSERT_EQUAL(1, indicator.showCount());
}

void test_clear_resets_indication() {
    FakeStatusIndicator indicator;
    indicator.show(ProvisioningIndication::ProvisioningSuccess);
    indicator.clear();

    TEST_ASSERT_FALSE(indicator.hasIndication());
}

void test_show_count_accumulates() {
    FakeStatusIndicator indicator;
    indicator.show(ProvisioningIndication::ProvisioningAvailable);
    indicator.show(ProvisioningIndication::AppConnected);
    indicator.show(ProvisioningIndication::ProvisioningFailure);

    TEST_ASSERT_EQUAL(3, indicator.showCount());
    TEST_ASSERT_EQUAL(static_cast<int>(ProvisioningIndication::ProvisioningFailure),
                       static_cast<int>(indicator.lastIndication()));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_no_indication_before_first_show);
    RUN_TEST(test_show_records_indication);
    RUN_TEST(test_clear_resets_indication);
    RUN_TEST(test_show_count_accumulates);
    return UNITY_END();
}
