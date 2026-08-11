# InterBridge Firmware Context

This file is the operational memory of this project. Read it before making
changes. **Every relevant implementation/change must update this file in
the same task** — do not treat documentation as separate work.

## Project Overview

InterBridge is firmware that bridges a traditional analog intercom with a
mobile app, intended to eventually run on an **ESP32-C3**. It will
detect ring/off-hook/on-hook on the physical intercom line, coordinate
audio between the intercom and the app, and communicate with an
InterBridge backend/app over Wi-Fi to allow remote call handling and
door release.

## Current Status

**Initial architectural foundation only.** No intercom, audio, or network
protocol behavior is functional yet — this stage intentionally builds a
clean, testable skeleton so firmware logic can be developed before the
final electronic circuit is available. Nothing in this codebase should be
treated as ready for the physical intercom.

## Architecture

See [`docs/architecture.md`](docs/architecture.md) for the full module
map, dependency direction, event flow, and design rationale. Summary:

- `core/` — events, state machine, logging, version. No Arduino
  dependency; fully unit tested on a host machine.
- `hardware/` — `IHardwareIO` interface + `Esp32GpioHardware` stub (HAL).
- `intercom/` — `LineDetector` (edge detection over `IHardwareIO`) and
  `Intercom` facade. No Arduino dependency; unit tested via a mock
  `IHardwareIO`.
- `audio/` — `IAudioIO` interface + `NullAudioIO` placeholder.
- `network/` — `wifi.*` (Wi-Fi station link, depends on Arduino `WiFi.h`)
  kept separate from `protocol.*` (`ICommunicationProtocol` interface +
  `NullProtocol` placeholder, transport-agnostic).
- `main.cpp` — thin composition root (`setup()`/`loop()`), no business
  logic.

The directory layout matches the structure requested for this task, with
one addition not in the original sketch: `core/logger.h/.cpp` and
`core/version.h`, added because logging and versioning are cross-cutting
concerns without an obvious home in `intercom/audio/network/hardware`.

## Implemented

- **Directory structure and PlatformIO project** (`platformio.ini`) with
  two environments: `esp32-c3` (real firmware target, generic
  `esp32-c3-devkitm-1` board) and `native` (host-side unit tests).
- **Event system** (`core/events.h/.cpp`): `EventType` enum
  (`RingDetected`, `OffHook`, `OnHook`, `DoorOpenRequested`,
  `CallStarted`, `CallEnded`, `WifiConnected`, `WifiDisconnected`) +
  `Event` struct + `toString()`.
- **State machine** (`core/state_machine.h/.cpp`): states `Boot`, `Idle`,
  `Ringing`, `InCall`, `Error`. Event-driven transitions for the basic
  call flow, an explicit `finishBoot()` (Boot → Idle), an explicit
  `reportFault()` (any state → Error), and an optional transition
  callback used for logging.
- **Logger** (`core/logger.h/.cpp`): structured `[INFO]/[WARN]/[ERROR]/
  [STATE]/[EVENT]` lines through a swappable sink (Serial on ESP32,
  stdout on native).
- **Hardware abstraction** (`hardware/gpio.h/.cpp`): `IHardwareIO`
  interface (`readLineState()`, `setDoorOutput()`) + `Esp32GpioHardware`,
  currently a **stub** (every method has a placeholder body — see
  Known Limitations).
- **Intercom abstraction** (`intercom/line_detector.h/.cpp`,
  `intercom/intercom.h/.cpp`): `LineDetector` turns raw
  `IHardwareIO::readLineState()` polling into `OffHook`/`OnHook` edge
  events; `Intercom` wraps it and exposes `requestDoorOpen()`.
- **Audio abstraction** (`audio/audio.h/.cpp`): `IAudioIO` interface +
  `NullAudioIO` no-op placeholder.
- **Network abstraction**: `network/wifi.h/.cpp` (`WifiManager`, wraps
  ESP32 `WiFi.h`, emits `WifiConnected`/`WifiDisconnected`) and
  `network/protocol.h/.cpp` (`ICommunicationProtocol` interface +
  `NullProtocol` placeholder).
- **`main.cpp`**: wires all modules together in `setup()`/`loop()` per
  the requested shape; contains no business logic.
- **Unit tests** (Unity, run under the `native` PlatformIO environment):
  state machine (11 cases), events (2 cases), line detector via a mock
  `IHardwareIO` (4 cases), `NullProtocol` (3 cases), `NullAudioIO`
  (3 cases). 23 assertions total, all passing — see Tests section below.
- **Documentation**: `docs/architecture.md` (architecture), `README.md`
  (project overview, build/test/upload instructions), this file.

## Decisions

### Decision: state machine transitions are event-driven, except boot and fault
`Boot → Idle` uses an explicit `finishBoot()` call (from `main.cpp`,
after all modules initialize) instead of a synthetic event, because
"initialization finished" is not one of the currently defined events.
Similarly, `reportFault()` forces a transition to `Error` from any state,
for use by modules that detect an unrecoverable problem.
Motivo: avoids inventing events (`BootCompleted`, `FaultDetected`) that
were not requested, while still keeping the state machine's public API
event-driven for the flows that were explicitly specified.
Consequência: if a future event type is added for boot completion or
faults, `finishBoot()`/`reportFault()` should be reconsidered — they may
become thin wrappers around `handleEvent()` instead of separate methods.

### Decision: `IHardwareIO` is a single, narrow interface
Only `readLineState()` and `setDoorOutput()` exist, both because those
are literally the only two capabilities requested in the task ("detectar
off-hook" and "abrir a porta") and because the real circuit is unknown.
Motivo: avoid inventing GPIO-shaped methods (`digitalRead(pin)`-style)
that would leak electrical assumptions into the interface.
Consequência: this interface will grow once the circuit is defined (e.g.
separate ring-detect vs. off-hook-detect methods, or a duration parameter
for door actuation). Expect additive changes, not a redesign — high-level
code already depends only on the interface, not on `Esp32GpioHardware`.

### Decision: ring detection is not implemented
`LineDetector` only distinguishes off-hook/on-hook (a single boolean
edge). It does **not** attempt to detect `RingDetected`.
Motivo: distinguishing a ring signal from an off-hook condition from a
single boolean read requires knowing the real line's electrical behavior
(e.g. a dedicated ring-detect signal, or AC ring waveform sampling),
which has not been characterized yet.
Consequência: `RingDetected` exists in `core/events.h` but nothing
produces it. `StateMachine` already handles it correctly (`Idle` →
`Ringing`) so no state machine change is needed once detection exists —
only `LineDetector`/`Intercom` need to grow.

### Decision: Wi-Fi (`network/wifi.*`) is allowed to depend on Arduino directly
Unlike `hardware/gpio.*`, `network/wifi.cpp` includes `WiFi.h` directly
instead of hiding it behind a from-scratch interface.
Motivo: Wi-Fi itself (as the transport to the backend/app) is a confirmed
requirement, not an unknown — unlike the intercom circuit or audio
hardware. Introducing an abstraction over a well-defined, single-vendor
API (`arduino-esp32`'s `WiFi.h`) would be premature abstraction.
Consequência: `network/wifi.cpp` is excluded from the `native` test
environment (see `platformio.ini`) because it cannot be compiled on a
host without the ESP32 Arduino core; it needs on-hardware validation.

### Decision: `WifiManager` and `ICommunicationProtocol` are separate modules
Motivo: "how the ESP32 joins the network" and "what it says to the
backend once connected" are independent decisions (explicitly called out
in the task instructions — do not choose MQTT/HTTP/WebSocket yet).
Consequência: `network/protocol.h` stays transport-agnostic and has no
Wi-Fi dependency, so it can be unit tested natively (see
`test/test_protocol`).

### Decision: `NullAudioIO` / `NullProtocol` placeholders instead of null pointers
Motivo: lets `main.cpp` and future coordinators hold a concrete object
today (no null checks scattered around) and swap it for a real
implementation later without changing call sites; also gives something
concrete and safe to unit test now (see `test/test_audio`,
`test/test_protocol`).
Consequência: none of their methods do anything real — this must not be
mistaken for a working audio or protocol implementation.

### Decision: `esp32-c3-devkitm-1` as the board in `platformio.ini`
Motivo: `platformio.ini` requires *some* concrete `board` value to build
the `esp32-c3` environment at all, and no specific dev board/module was
provided. `esp32-c3-devkitm-1` is Espressif's own generic ESP32-C3 dev
board, used purely as a placeholder that lets the project build.
Consequência: **provisional.** Must be updated once the real board/module
for InterBridge is chosen — this may also affect flash size, PSRAM
availability, and default pin mapping assumptions.

## Known Limitations

- `Esp32GpioHardware` (`hardware/gpio.cpp`) is a stub: `readLineState()`
  always returns `false`, `setDoorOutput()` does nothing. No GPIOs are
  configured. This is intentional — the electrical interface is not
  defined yet — but it means the firmware currently cannot observe or
  affect the real intercom in any way.
- `LineDetector` cannot detect ring signals (see Decisions above).
- `Intercom::requestDoorOpen()` just sets the (stub) output high and
  never turns it back off — real actuation (pulse duration, latching,
  active level) is undefined.
- `WifiManager::begin()` is never called with real credentials in
  `main.cpp` — there is no defined provisioning mechanism yet, so
  `initializeNetwork()` only logs and does not attempt to connect.
- `NullAudioIO` and `NullProtocol` are non-functional by design.
- No watchdog, OTA, or persistent configuration storage exists yet.
- This environment has no PlatformIO, no gcc/clang, and no internet-free
  way to fetch the Espressif ESP32-C3 toolchain, so the `esp32-c3`
  environment has **not** been built with the real toolchain — see Tests
  section for exactly what verification was possible.

## Open Questions

- Exact ESP32-C3 development board/module to design around (affects
  `platformio.ini`'s `board` value, available pins, flash/PSRAM size).
- Intercom line interface circuit: how is the line electrically sampled
  by the ESP32 (voltage levels, isolation/protection components)?
- How is off-hook actually detected (voltage threshold? current sense?
  a dedicated relay/optocoupler signal?).
- How is ring detected, and is it distinguishable from off-hook on the
  chosen circuit?
- How is the door release actually driven (relay vs. transistor, pulse
  duration, active-high vs. active-low, latching vs. momentary)?
- Audio hardware: microphone/speaker or line-level interface with the
  intercom? I2S vs. analog? Any codec chip involved?
- Audio codec/format and how audio is transported between ESP32 and the
  app (embedded in the same protocol? a separate stream?).
- Application protocol between ESP32 and InterBridge backend/app (MQTT,
  HTTP, WebSocket, custom TCP, ...).
- Authentication/pairing mechanism between the ESP32 and the app/backend.
- Wi-Fi credential provisioning mechanism (hardcoded for bring-up? NVS
  config? BLE/captive-portal provisioning?).
- OTA update strategy, if any.
- Watchdog/recovery strategy for the `Error` state (currently a dead end
  with no transition out).

## Technical Debt

- `main.cpp` never actually connects Wi-Fi (no credentials source) —
  `WifiManager` exists but is unused beyond construction. Revisit once
  provisioning is decided.
- `Intercom::requestDoorOpen()` has no timing/pulse logic — will need
  revisiting once the door-release circuit is defined, likely requiring
  a duration parameter or a follow-up "close output" call.
- `StateMachine::updateStateMachine()` in `main.cpp` is currently a
  no-op placeholder for future timeout/retry logic (e.g. a ringing
  timeout). Not implemented because no timeout behavior was specified.
- `docs/architecture.md` and this file must be kept manually in sync with
  the code; there is no automated check for drift yet.

## Future Work

- Characterize the real intercom line electrically, then implement
  `Esp32GpioHardware::readLineState()`/`setDoorOutput()` for real.
- Implement ring detection once the electrical distinction between ring
  and off-hook is known; wire it into `LineDetector`/`Intercom` to
  produce `RingDetected`.
- Decide and implement Wi-Fi credential provisioning; wire
  `WifiManager::begin()` into `initializeNetwork()`.
- Choose the application protocol and implement a concrete
  `ICommunicationProtocol`, replacing `NullProtocol`.
- Choose audio hardware/codec and implement a concrete `IAudioIO`,
  replacing `NullAudioIO`; connect it to `Intercom` for call audio.
- Add `CONNECTING_WIFI`, `READY`, `OPENING_DOOR`, `RECOVERING` states
  once the corresponding subsystems (network, door actuation, error
  recovery) have real behavior to model — do not add them speculatively.
- Add a watchdog/recovery strategy and a real exit path from `Error`.
- Once real PlatformIO/toolchain access is available in a build
  environment, run `pio run -e esp32-c3` and `pio test -e native` as the
  authoritative build/test verification (see Tests section for what
  substituted for this here).

## Tests

Executed in this session (see also `test/`):

| Test dir | Covers | Assertions | Result |
|---|---|---|---|
| `test_state_machine` | Boot/Idle/Ringing/InCall/Error transitions, invalid events, fault reporting, transition callback | 11 | Pass |
| `test_events` | `Event` construction, `toString()` for all `EventType` values | 2 | Pass |
| `test_line_detector` | Off-hook/on-hook edge detection via a mock `IHardwareIO` | 4 | Pass |
| `test_protocol` | `NullProtocol` safe-default behavior | 3 | Pass |
| `test_audio` | `NullAudioIO` safe-default behavior | 3 | Pass |

**Needs validation (could not be run here):** this environment has no
PlatformIO, gcc, or clang installed, and installing the full Espressif
ESP32-C3 toolchain was not attempted (large download, out of scope for a
one-off check). As a substitute:
- All `native`-environment source files and all five test files above
  were compiled **and executed** with MSVC (`cl.exe` from VS 2022 Build
  Tools) against a local, throwaway Unity-compatible macro shim (not
  part of the repo) — all 23 assertions passed.
- `src/main.cpp` and `src/network/wifi.cpp` (the two Arduino-dependent
  files, normally excluded from the `native` environment) were
  additionally syntax/semantic-checked with MSVC against minimal
  `Arduino.h`/`WiFi.h` shims (also not part of the repo) — no errors.
- **Not verified:** an actual `pio run -e esp32-c3` build against the
  real `espressif32` platform/toolchain, and `pio test -e native` via
  PlatformIO+Unity+gcc specifically. The `platformio.ini` configuration
  itself (env names, `build_src_filter`, board id) has not been executed
  by PlatformIO. Do this before relying on the `esp32-c3` environment.
- **Hardware-dependent, not testable at all yet:** anything exercising
  `Esp32GpioHardware`, real Wi-Fi connectivity, or the physical intercom
  line — these require the real board and circuit.

## Hardware Dependencies

- `Esp32GpioHardware` (off-hook detection, door output) — needs the
  intercom interface circuit to be designed first.
- `WifiManager` — functionally complete as a wrapper, but connecting to a
  real access point needs to be verified on the real ESP32-C3 board.
- Any future `IAudioIO` implementation — needs audio hardware to be
  selected.
- The `esp32-c3` PlatformIO environment itself — needs to be built and
  flashed on real hardware to confirm `platformio.ini` (board id, flags)
  is correct; only syntax-level verification was possible in this
  session (see Tests).

## Change Log

### 2026-08-11

Implemented:
- Initial project structure (`src/core`, `src/hardware`, `src/intercom`,
  `src/audio`, `src/network`, `include/`, `test/`, `docs/`).
- `platformio.ini` with `esp32-c3` and `native` environments.
- Event system (`core/events.*`) and state machine (`core/state_machine.*`)
  with Boot/Idle/Ringing/InCall/Error and an event-driven core call flow.
- Structured logger (`core/logger.*`) with a swappable sink.
- Hardware abstraction (`hardware/gpio.*`): `IHardwareIO` + stub
  `Esp32GpioHardware`.
- Intercom abstraction (`intercom/line_detector.*`, `intercom/intercom.*`)
  with off-hook/on-hook edge detection over `IHardwareIO`.
- Audio abstraction (`audio/audio.*`): `IAudioIO` + `NullAudioIO`.
- Network abstraction: `network/wifi.*` (`WifiManager`) and
  `network/protocol.*` (`ICommunicationProtocol` + `NullProtocol`).
- `main.cpp` composition root wiring all modules together.
- Unit tests for state machine, events, line detector (via mock
  hardware), `NullProtocol`, `NullAudioIO` — 23 assertions, all passing
  (verified via MSVC + a local Unity-compatible shim; see Tests).
- `README.md` and `docs/architecture.md`.

Still needed because of this change:
- Everything under Open Questions above — none of the hardware/protocol
  unknowns were resolved, by design.
- Actual verification via PlatformIO + the real `espressif32` toolchain
  (not available in this session).
- Real GPIO mapping and `Esp32GpioHardware` implementation.
- Ring detection.
- Wi-Fi provisioning wired into `main.cpp`.
- A concrete communication protocol and audio implementation.

Next steps:
- Run `pio run -e esp32-c3` and `pio test -e native` in an environment
  with PlatformIO installed, and fix anything that surfaces (this
  session only had MSVC available as a substitute check — see Tests).
- Define the intercom line interface circuit, then implement
  `Esp32GpioHardware` for real and validate `LineDetector` against it.
- Decide the Wi-Fi provisioning mechanism and wire `WifiManager` into
  `main.cpp`.
- Decide the application protocol and audio hardware/codec.
