#pragma once

#include <cstdint>

namespace interbridge {

// esp_err_t is just a typedef for `int32_t` in ESP-IDF (esp_err.h). Using
// that type directly here (instead of esp_err_t) keeps this header
// includable from native tests without pulling in any ESP-IDF header -
// the meter firmware passes real esp_err_t values in unchanged.
using ClockProbeEspErr = int32_t;
constexpr ClockProbeEspErr kClockProbeEspOk = 0; // ESP_OK

// ESP-IDF's own driver/pcnt.h documents pcnt_isr_service_install()'s
// ESP_ERR_INVALID_STATE return specifically as "ISR service already
// installed" - not a generic failure. This mirrors that documented
// numeric value (ESP_ERR_INVALID_STATE = 0x103, esp_err.h) so the
// decision below is testable without any ESP-IDF header.
constexpr ClockProbeEspErr kClockProbeEspErrInvalidState = 0x103;

// Whether a pcnt_isr_service_install() return value means the ISR
// service is ready to use: either it was just installed (ESP_OK), or it
// was already installed by an earlier call in this same firmware
// (ESP_ERR_INVALID_STATE, per that function's own documented contract -
// this is not swallowing a generic error, it is the one specific code
// the driver documents for this exact situation). Any other code is a
// real failure and must not be treated as "ready".
bool isPcntIsrServiceReady(ClockProbeEspErr result);

// Tracks the ordered PCNT bring-up sequence (see
// si3050_clock_probe_meter_main.cpp) and its first failure, if any.
// Hardware-independent: the real meter firmware calls record() with the
// esp_err_t returned by each actual ESP-IDF call, in order; this class
// never touches a peripheral itself, so bring-up correctness on real
// hardware is NOT proven by native tests - see
// docs/si3050-clock-probe.md.
class PcntBringupTracker {
public:
    // Records one step's outcome. A step "succeeds" when
    // isPcntIsrServiceReady()-equivalent logic is not needed here -
    // callers pass kClockProbeEspOk for a plain success, or the already-
    // classified boolean result of isPcntIsrServiceReady() for the ISR
    // install step specifically (see the firmware). Once a failure has
    // been recorded, further record() calls are ignored: the firmware
    // must stop issuing real PCNT calls after the first failure, so only
    // the first (root-cause) failure is kept.
    void record(const char* stepName, ClockProbeEspErr result, bool succeeded);

    bool hasFailed() const;
    const char* failedStepName() const; // nullptr if hasFailed() is false
    ClockProbeEspErr failedStepResult() const;

private:
    bool hasFailed_ = false;
    const char* failedStepName_ = nullptr;
    ClockProbeEspErr failedStepResult_ = kClockProbeEspOk;
};

} // namespace interbridge
