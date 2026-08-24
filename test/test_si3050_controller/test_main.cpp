#include <unity.h>

#include <algorithm>
#include <cstddef>
#include <optional>

#include "intercom/si3050/si3050_bus.h"
#include "intercom/si3050/si3050_call_log.h"
#include "intercom/si3050/si3050_config.h"
#include "intercom/si3050/si3050_controller.h"
#include "intercom/si3050/si3050_delay.h"
#include "intercom/si3050/si3050_pcm_clock.h"
#include "intercom/si3050/si3050_pins.h"
#include "intercom/si3050/si3050_reset.h"
#include "intercom/si3050/si3050_timing.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

// ---- Pin map / compile-time contract ----

void test_gpio8_and_gpio9_remain_reserved_per_contract() {
    // GPIO8 is committed to /RGDT; GPIO9 stays reserved for BOOT and must
    // never be claimed by the Si3050 driver.
    TEST_ASSERT_EQUAL(8, kSi3050PinRgdt);
    TEST_ASSERT_EQUAL(9, kSi3050ReservedPinBoot);
    TEST_ASSERT_TRUE(si3050_detail::noReservedPinOverlap());
}

void test_si3050_pins_do_not_collide_with_usb_button_or_led() {
    TEST_ASSERT_FALSE(si3050_detail::contains(si3050_detail::kSi3050Pins, si3050_detail::kSi3050PinCount,
                                              kSi3050ReservedPinUsbDMinus));
    TEST_ASSERT_FALSE(si3050_detail::contains(si3050_detail::kSi3050Pins, si3050_detail::kSi3050PinCount,
                                              kSi3050ReservedPinUsbDPlus));
    TEST_ASSERT_FALSE(
        si3050_detail::contains(si3050_detail::kSi3050Pins, si3050_detail::kSi3050PinCount, kSi3050ReservedPinButton));
    TEST_ASSERT_FALSE(si3050_detail::contains(si3050_detail::kSi3050Pins, si3050_detail::kSi3050PinCount,
                                              kSi3050ReservedPinStatusLed));
}

// ---- Timing helpers (datasheet-derived formulas, not guessed) ----

void test_cycles_to_microseconds_rounds_up_to_whole_microsecond() {
    // 10 cycles @ 2.048 MHz = 4.8828125 us -> ceil = 5.
    TEST_ASSERT_EQUAL(5, si3050CyclesToMicroseconds(10, 2048000));
}

void test_pll_settle_matches_datasheet_formula() {
    // Tsettle = 64 / FPCLK (datasheet Section 5.30). At 2.048 MHz:
    // 64 / 2,048,000 = 31.25 us -> ceil = 32.
    TEST_ASSERT_EQUAL(32, si3050PllSettleMicroseconds(2048000));
}

// ---- Si3050Controller bring-up sequence ----

void test_cs_born_deselected() {
    FakeSi3050Bus bus;
    TEST_ASSERT_FALSE(bus.chipSelected);
}

void test_no_spi_transaction_before_ready() {
    FakeSi3050Bus bus;
    FakePcmClock clock;
    FakeSi3050Reset reset;
    FakeDelayProvider delay;
    Si3050Controller controller(bus, clock, reset, delay);

    std::optional<uint8_t> result = controller.transferRaw(0x00);

    TEST_ASSERT_FALSE(result.has_value());
    TEST_ASSERT_EQUAL(0, bus.transferCallCount);
    TEST_ASSERT_FALSE(controller.isReady());
}

void test_initialize_follows_documented_electrical_sequence() {
    Si3050CallLog log;
    FakeSi3050Bus bus(&log);
    FakePcmClock clock(&log);
    FakeSi3050Reset reset(&log);
    FakeDelayProvider delay(&log);
    Si3050Controller controller(bus, clock, reset, delay);

    controller.initialize();

    const char* expected[] = {
        "bus.begin", "bus.cs.deselect", "reset.assert", "bus.sclkIdleHigh",
        "clock.start", "delay.wait", "reset.release", "delay.wait",
    };
    TEST_ASSERT_EQUAL(sizeof(expected) / sizeof(expected[0]), log.size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(expected[i], log[i].c_str());
    }

    TEST_ASSERT_TRUE(controller.isReady());
    TEST_ASSERT_TRUE(bus.beginCalled);
    TEST_ASSERT_TRUE(bus.sclkHeldHigh);
    TEST_ASSERT_FALSE(bus.chipSelected);
    TEST_ASSERT_TRUE(clock.isRunning());
    TEST_ASSERT_FALSE(reset.isAsserted());
}

void test_sclk_held_high_before_reset_release() {
    Si3050CallLog log;
    FakeSi3050Bus bus(&log);
    FakePcmClock clock(&log);
    FakeSi3050Reset reset(&log);
    FakeDelayProvider delay(&log);
    Si3050Controller controller(bus, clock, reset, delay);

    controller.initialize();

    auto sclkIt = std::find(log.begin(), log.end(), "bus.sclkIdleHigh");
    auto resetReleaseIt = std::find(log.begin(), log.end(), "reset.release");
    TEST_ASSERT_TRUE(sclkIt != log.end());
    TEST_ASSERT_TRUE(resetReleaseIt != log.end());
    TEST_ASSERT_TRUE(sclkIt < resetReleaseIt);
}

void test_pclk_fsync_started_before_reset_release() {
    Si3050CallLog log;
    FakeSi3050Bus bus(&log);
    FakePcmClock clock(&log);
    FakeSi3050Reset reset(&log);
    FakeDelayProvider delay(&log);
    Si3050Controller controller(bus, clock, reset, delay);

    controller.initialize();

    auto clockStartIt = std::find(log.begin(), log.end(), "clock.start");
    auto resetReleaseIt = std::find(log.begin(), log.end(), "reset.release");
    TEST_ASSERT_TRUE(clockStartIt != log.end());
    TEST_ASSERT_TRUE(resetReleaseIt != log.end());
    TEST_ASSERT_TRUE(clockStartIt < resetReleaseIt);
}

void test_minimum_ten_cycle_wait_precedes_reset_release() {
    FakeSi3050Bus bus;
    FakePcmClock clock;
    FakeSi3050Reset reset;
    FakeDelayProvider delay;
    Si3050Config config; // default pclkHz = 2048000
    Si3050Controller controller(bus, clock, reset, delay, config);

    controller.initialize();

    TEST_ASSERT_EQUAL(2, delay.calls.size());
    uint32_t expectedMinWait = si3050CyclesToMicroseconds(kSi3050MinPclkCyclesBeforeResetRelease, config.pclkHz);
    TEST_ASSERT_EQUAL(expectedMinWait, delay.calls[0]);
    uint32_t expectedPllSettle = si3050PllSettleMicroseconds(config.pclkHz);
    TEST_ASSERT_EQUAL(expectedPllSettle, delay.calls[1]);
}

void test_initialize_is_idempotent() {
    Si3050CallLog log;
    FakeSi3050Bus bus(&log);
    FakePcmClock clock(&log);
    FakeSi3050Reset reset(&log);
    FakeDelayProvider delay(&log);
    Si3050Controller controller(bus, clock, reset, delay);

    controller.initialize();
    std::size_t sizeAfterFirst = log.size();
    controller.initialize(); // must be a no-op - never re-runs bring-up

    TEST_ASSERT_EQUAL(sizeAfterFirst, log.size());
}

void test_spi_read_via_controller_after_ready() {
    FakeSi3050Bus bus;
    FakePcmClock clock;
    FakeSi3050Reset reset;
    FakeDelayProvider delay;
    Si3050Controller controller(bus, clock, reset, delay);

    controller.initialize();
    TEST_ASSERT_TRUE(controller.isReady());

    bus.nextTransferReturn = 0xAB;
    std::optional<uint8_t> result = controller.transferRaw(0x00);

    TEST_ASSERT_TRUE(result.has_value());
    TEST_ASSERT_EQUAL_HEX8(0xAB, *result);
    TEST_ASSERT_EQUAL(1, bus.transferCallCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_gpio8_and_gpio9_remain_reserved_per_contract);
    RUN_TEST(test_si3050_pins_do_not_collide_with_usb_button_or_led);
    RUN_TEST(test_cycles_to_microseconds_rounds_up_to_whole_microsecond);
    RUN_TEST(test_pll_settle_matches_datasheet_formula);
    RUN_TEST(test_cs_born_deselected);
    RUN_TEST(test_no_spi_transaction_before_ready);
    RUN_TEST(test_initialize_follows_documented_electrical_sequence);
    RUN_TEST(test_sclk_held_high_before_reset_release);
    RUN_TEST(test_pclk_fsync_started_before_reset_release);
    RUN_TEST(test_minimum_ten_cycle_wait_precedes_reset_release);
    RUN_TEST(test_initialize_is_idempotent);
    RUN_TEST(test_spi_read_via_controller_after_ready);
    return UNITY_END();
}
