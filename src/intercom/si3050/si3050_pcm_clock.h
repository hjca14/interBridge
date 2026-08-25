#pragma once

#include <cstdint>

#include "si3050_call_log.h"

namespace interbridge {

// PCM clock generation (PCLK + FSYNC) for the Si3050. Target rates for
// Rev A: PCLK = 1.024 MHz, FSYNC = 8 kHz (see si3050_config.h - PCM/SPI
// mode, the mode InterBridge uses, not the GCI mode's stricter 2.048/
// 4.096 MHz requirement). The Si3050 datasheet's "Clock Generation"
// section (cited by title, not section number - numbering varies
// between datasheet revisions) requires PCLK to run at one of a fixed
// set of rates synchronous to an 8 kHz FSYNC.
class IPcmClock {
public:
    virtual ~IPcmClock() = default;

    virtual void start(uint32_t pclkHz, uint32_t fsyncHz) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

// TDM slot geometry Esp32PcmClock configures the ESP32-C3's I2S
// peripheral for: 16 timeslots x 8 bits/timeslot = 128 PCLK cycles per
// FSYNC frame, matching the Si3050's PCM/SPI-mode PCM Highway geometry
// exactly. This is not a guess: it is the same configuration physically
// validated on real ESP32-C3 hardware (measured externally by an
// independent PCNT-based meter board, never self-measured) in the Phase
// 3B.1 clock probe bench experiment - see docs/si3050-clock-probe.md's
// "Real bench observation: 16 x 8 slot geometry reaches the PCM/SPI
// target" for the full physical result
// (`pclk_hz ~= 1,024,100`, `fsync_hz ~= 8,001`-`8,002`, `ratio ~=
// 127.98`-`128.00`). That probe is a separate, isolated bench
// environment (`esp32-c3-si3050-clock-probe`) - these constants are a
// deliberate, minimal restatement of its validated geometry in
// production code, not a dependency on the probe's own `src/dev/`
// module (which stays bench-only).
constexpr uint32_t kSi3050PcmTdmSlotCount = 16;
constexpr uint32_t kSi3050PcmTdmSlotWidthBits = 8;

// PCLK cycles per FSYNC frame for the given TDM geometry - matches what
// src/dev/si3050_clock_probe_generator_config.h's configuredTdmRatio()
// computes for the probe, restated here so production code has no
// dependency on src/dev/.
constexpr uint32_t si3050PcmClocksPerFrame(uint32_t slotCount, uint32_t slotWidthBits) {
    return slotCount * slotWidthBits;
}

// The PCLK rate Esp32PcmClock's fixed TDM geometry implies for a given
// FSYNC rate - fsyncHz * si3050PcmClocksPerFrame(kSi3050PcmTdmSlotCount,
// kSi3050PcmTdmSlotWidthBits).
constexpr uint32_t si3050PcmRequestedPclkHz(uint32_t fsyncHz, uint32_t slotCount, uint32_t slotWidthBits) {
    return fsyncHz * si3050PcmClocksPerFrame(slotCount, slotWidthBits);
}

// Esp32PcmClock's fixed 16 x 8 TDM geometry can only honor a start()
// request whose pclkHz is exactly what that geometry implies for the
// requested fsyncHz - this is a fail-closed config gate, not a
// derivation: start() never silently substitutes a different pclkHz
// than the caller asked for.
constexpr bool si3050PcmConfigurationSupported(uint32_t pclkHz, uint32_t fsyncHz) {
    return fsyncHz != 0 &&
           pclkHz == si3050PcmRequestedPclkHz(fsyncHz, kSi3050PcmTdmSlotCount, kSi3050PcmTdmSlotWidthBits);
}

// esp_err_t is just a typedef for `int32_t` in ESP-IDF (esp_err.h). Using
// that type directly here (instead of esp_err_t) keeps this header
// includable from native tests without pulling in any ESP-IDF header -
// see the identical rationale in
// src/dev/si3050_clock_probe_meter_bringup.h (a separate, dev-only type,
// not shared with this production one, so this module has no dependency
// on src/dev/).
using Si3050PcmClockEspErr = int32_t;
constexpr Si3050PcmClockEspErr kSi3050PcmClockEspOk = 0;

// Pure, hardware-independent bring-up/rollback/idempotency decision
// logic for Esp32PcmClock::start()/stop(). Never touches a peripheral
// itself: Esp32PcmClock calls its record*() methods with the real
// esp_err_t returned by each actual ESP-IDF call, in order, and asks it
// what to do next (shouldStart()/shouldUninstall()) before making any
// hardware call - so bring-up/teardown correctness on real hardware is
// NOT proven by native tests of this class alone, only this decision
// logic is (mirrors PcntBringupTracker's role for the clock probe
// meter - see src/dev/si3050_clock_probe_meter_bringup.h - but is a
// separate class so this production module has no dependency on
// src/dev/).
class Si3050PcmClockBringup {
public:
    // True if start() should attempt a fresh install sequence (false =
    // already running - start() must be idempotent and must not touch
    // hardware again).
    bool shouldStart() const;

    // Step 1: i2s_driver_install(). Tracked separately from the other
    // steps because only a successful install means a real
    // i2s_driver_uninstall() is later owed (see shouldUninstall()).
    void recordDriverInstall(Si3050PcmClockEspErr result, bool succeeded);

    // Steps 2+ (i2s_set_pin(), i2s_zero_dma_buffer(), ...): first
    // failure wins, matching PcntBringupTracker's contract - once a
    // failure has been recorded, further record() calls for this
    // attempt are ignored.
    void record(const char* stepName, Si3050PcmClockEspErr result, bool succeeded);

    // Call once every step for this attempt has succeeded - marks the
    // clock genuinely running. A no-op if a failure was already
    // recorded.
    void markRunning();

    bool isRunning() const;
    bool hasFailed() const;
    const char* failedStepName() const; // nullptr if hasFailed() is false
    Si3050PcmClockEspErr failedStepResult() const;

    // True if a real i2s_driver_uninstall() call is owed right now: the
    // driver was installed by recordDriverInstall() and has not yet
    // been rolled back (on a later failure) or stopped. True both while
    // genuinely running and mid-failure after a successful install -
    // both cases mean the driver resource is still held.
    bool shouldUninstall() const;

    // Call once the caller has actually issued i2s_driver_uninstall() -
    // whether as a failure rollback or a real stop() - to reset all
    // state for the next start(). Safe to call even when nothing was
    // installed (stop() on an already-stopped clock).
    void recordUninstalled();

private:
    bool running_ = false;
    bool driverInstalled_ = false;
    bool hasFailed_ = false;
    const char* failedStepName_ = nullptr;
    Si3050PcmClockEspErr failedStepResult_ = kSi3050PcmClockEspOk;
};

// Real ESP32-C3 implementation for GPIO0 (PCLK) / GPIO1 (FSYNC) - see
// si3050_pins.h. Configures the ESP32-C3's I2S peripheral as a hardware
// TDM master (16 x 8 slots, PCM short format - see
// kSi3050PcmTdmSlotCount/kSi3050PcmTdmSlotWidthBits above), the exact
// geometry physically validated by the Phase 3B.1 clock probe. This
// generates the clock signal only - it does not configure or exchange
// any PCM audio data (DRX/DTX are left unrouted, matching the probe),
// and it has not been exercised against a real Si3050 part (none has
// been connected). Every driver call's esp_err_t is checked via
// Si3050PcmClockBringup; on any failure, start() fails closed
// (isRunning() stays false) and rolls back whatever this call actually
// acquired.
class Esp32PcmClock : public IPcmClock {
public:
    void start(uint32_t pclkHz, uint32_t fsyncHz) override;
    void stop() override;
    bool isRunning() const override;

private:
    Si3050PcmClockBringup bringup_;
};

// Deterministic test double.
class FakePcmClock : public IPcmClock {
public:
    explicit FakePcmClock(Si3050CallLog* log = nullptr) : log_(log) {}

    void start(uint32_t pclkHz, uint32_t fsyncHz) override;
    void stop() override;
    bool isRunning() const override;

    uint32_t lastPclkHz = 0;
    uint32_t lastFsyncHz = 0;
    // Lets a test simulate a clock that was asked to start but never
    // actually came up (e.g. a real bring-up failure, or an unsupported
    // pclkHz/fsyncHz pair - see Esp32PcmClock/si3050PcmConfigurationSupported())
    // - start() sets running_ from this instead of unconditionally true.
    // Defaults to true so every other test's FakePcmClock behaves like a
    // working clock without opting in.
    bool startSucceeds = true;

private:
    void log(const char* tag);
    bool running_ = false;
    Si3050CallLog* log_;
};

} // namespace interbridge
