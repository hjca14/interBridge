#ifndef INTERBRIDGE_SI3050_CLOCK_PROBE_METER
#error "si3050_clock_probe_meter_main.cpp is only for INTERBRIDGE_SI3050_CLOCK_PROBE_METER"
#endif

// Phase 3B.1 bench-only experiment: measures the PCLK/FSYNC clocks
// produced by esp32-c3-si3050-clock-probe, via hardware pulse counting
// (PCNT) on a classic ESP32 DevKitV1 - never digitalRead()/GPIO
// interrupts for the pulses themselves. Wiring:
//   ESP32-C3 GPIO0 (PCLK)  -> DevKitV1 GPIO34
//   ESP32-C3 GPIO1 (FSYNC) -> DevKitV1 GPIO35
//   ESP32-C3 GND           -> DevKitV1 GND
// Do not tie the two boards' 3V3 rails together - each is USB-powered
// independently. See docs/si3050-clock-probe.md.
//
// This firmware only measures frequency and the PCLK:FSYNC ratio. It
// never declares an absolute "PASS" - it does not prove amplitude,
// noise, duty cycle, edge integrity, or fine alignment between PCLK and
// FSYNC. No Wi-Fi, MQTT, or door-actuation dependency.

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include "driver/pcnt.h"

#include "si3050_clock_probe_math.h"

using namespace interbridge;

namespace {

constexpr gpio_num_t kPclkInputPin = GPIO_NUM_34;
constexpr gpio_num_t kFsyncInputPin = GPIO_NUM_35;

constexpr pcnt_unit_t kPclkUnit = PCNT_UNIT_0;
constexpr pcnt_unit_t kFsyncUnit = PCNT_UNIT_1;

constexpr uint32_t kReportIntervalMs = 1000;

portMUX_TYPE g_pcntMux = portMUX_INITIALIZER_UNLOCKED;
// Guarded by g_pcntMux for any access outside the ISR that fires it.
volatile uint32_t g_pclkOverflowCount = 0;
volatile uint32_t g_fsyncOverflowCount = 0;

ClockProbeMinMaxTracker g_pclkHzStats;
ClockProbeMinMaxTracker g_fsyncHzStats;
ClockProbeMinMaxTracker g_ratioStats;

uint32_t g_windowStartMs = 0;
int64_t g_windowStartMicros = 0;

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

// Shared ISR for both PCNT units: fires only on PCNT_EVT_H_LIM (the only
// event enabled below). Increments the software overflow counter for
// whichever unit triggered it and resets that unit's hardware counter to
// 0, so the counter never wraps within an int16_t before the next
// reporting window reads it - see combinePulseCount()'s contract.
void IRAM_ATTR onPcntOverflow(void* arg) {
    const pcnt_unit_t unit = static_cast<pcnt_unit_t>(reinterpret_cast<uintptr_t>(arg));
    uint32_t status = 0;
    pcnt_get_event_status(unit, &status);
    if (!(status & PCNT_EVT_H_LIM)) return;

    portENTER_CRITICAL_ISR(&g_pcntMux);
    if (unit == kPclkUnit) {
        ++g_pclkOverflowCount;
    } else {
        ++g_fsyncOverflowCount;
    }
    pcnt_counter_clear(unit);
    portEXIT_CRITICAL_ISR(&g_pcntMux);
}

void configureUnit(pcnt_unit_t unit, gpio_num_t pin) {
    pcnt_config_t config = {};
    config.pulse_gpio_num = static_cast<int>(pin);
    config.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    config.channel = PCNT_CHANNEL_0;
    config.unit = unit;
    config.pos_mode = PCNT_COUNT_INC; // count rising edges
    config.neg_mode = PCNT_COUNT_DIS; // ignore falling edges
    config.lctrl_mode = PCNT_MODE_KEEP;
    config.hctrl_mode = PCNT_MODE_KEEP;
    config.counter_h_lim = static_cast<int16_t>(kClockProbePcntHighLimit);
    config.counter_l_lim = 0;

    pcnt_unit_config(&config);

    // Deliberately no glitch filter: pcnt_filter_enable()/
    // pcnt_set_filter_value() are never called. The filter is measured
    // in APB clock cycles and could silently eat legitimate ~244 ns
    // half-cycles at a 2.048 MHz PCLK.
    pcnt_filter_disable(unit);

    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_event_enable(unit, PCNT_EVT_H_LIM);
    pcnt_counter_resume(unit);
}

// Atomically reads this window's (overflowCount, rawCount) and resets
// both the software overflow accumulator and the hardware counter for
// the next window, all with the ISR excluded - so a torn read/reset
// (the ISR firing mid-sample) cannot happen.
void sampleAndResetUnit(pcnt_unit_t unit, volatile uint32_t& overflowCounter, uint32_t& overflowOut,
                        int32_t& rawOut) {
    int16_t raw = 0;
    portENTER_CRITICAL(&g_pcntMux);
    overflowOut = overflowCounter;
    overflowCounter = 0;
    pcnt_get_counter_value(unit, &raw);
    pcnt_counter_clear(unit);
    portEXIT_CRITICAL(&g_pcntMux);
    rawOut = raw;
}

} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + 3000;
    while (!Serial && !deadlineReached(millis(), serialDeadline)) delay(10);

    pcnt_isr_service_install(0);
    configureUnit(kPclkUnit, kPclkInputPin);
    configureUnit(kFsyncUnit, kFsyncInputPin);
    pcnt_isr_handler_add(kPclkUnit, onPcntOverflow, reinterpret_cast<void*>(static_cast<uintptr_t>(kPclkUnit)));
    pcnt_isr_handler_add(kFsyncUnit, onPcntOverflow, reinterpret_cast<void*>(static_cast<uintptr_t>(kFsyncUnit)));

    Serial.printf("[SI3050 CLOCK METER] pcnt configured pclk_pin=%d fsync_pin=%d h_lim=%ld\n",
                 static_cast<int>(kPclkInputPin), static_cast<int>(kFsyncInputPin),
                 static_cast<long>(kClockProbePcntHighLimit));

    g_windowStartMs = millis();
    g_windowStartMicros = esp_timer_get_time();
}

void loop() {
    const uint32_t now = millis();
    if (!deadlineReached(now, g_windowStartMs + kReportIntervalMs)) {
        delay(5); // brief yield only - the reported window duration below comes from esp_timer_get_time(), not this delay
        return;
    }

    const int64_t windowEndMicros = esp_timer_get_time();
    const uint64_t windowMicros = static_cast<uint64_t>(windowEndMicros - g_windowStartMicros);

    uint32_t pclkOverflow = 0;
    int32_t pclkRaw = 0;
    sampleAndResetUnit(kPclkUnit, g_pclkOverflowCount, pclkOverflow, pclkRaw);

    uint32_t fsyncOverflow = 0;
    int32_t fsyncRaw = 0;
    sampleAndResetUnit(kFsyncUnit, g_fsyncOverflowCount, fsyncOverflow, fsyncRaw);

    const uint64_t pclkEdges = combinePulseCount(pclkOverflow, pclkRaw, kClockProbePcntHighLimit);
    const uint64_t fsyncEdges = combinePulseCount(fsyncOverflow, fsyncRaw, kClockProbePcntHighLimit);

    const ClockProbeWindowResult result = computeClockProbeWindowResult(windowMicros, pclkEdges, fsyncEdges);

    g_pclkHzStats.observe(result.pclkHz);
    g_fsyncHzStats.observe(result.fsyncHz);
    g_ratioStats.observe(result.ratio);

    Serial.printf(
        "[SI3050 CLOCK METER] window_us=%llu pclk_edges=%llu pclk_hz=%.1f fsync_edges=%llu fsync_hz=%.2f ratio=%.3f\n",
        static_cast<unsigned long long>(result.windowMicros), static_cast<unsigned long long>(result.pclkEdges),
        result.pclkHz, static_cast<unsigned long long>(result.fsyncEdges), result.fsyncHz, result.ratio);
    Serial.printf(
        "[SI3050 CLOCK METER] stats pclk_hz_min=%.1f pclk_hz_max=%.1f fsync_hz_min=%.2f fsync_hz_max=%.2f "
        "ratio_min=%.3f ratio_max=%.3f\n",
        g_pclkHzStats.minValue(), g_pclkHzStats.maxValue(), g_fsyncHzStats.minValue(), g_fsyncHzStats.maxValue(),
        g_ratioStats.minValue(), g_ratioStats.maxValue());

    g_windowStartMs = now;
    g_windowStartMicros = windowEndMicros;
}
