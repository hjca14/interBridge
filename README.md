# InterBridge Firmware

Firmware for **InterBridge**, a bridge between a traditional analog
intercom and a mobile app, intended to eventually run on an **ESP32-C3**.

This repository currently contains the initial architectural foundation
of the firmware — a clean, testable, extensible skeleton — and **not**
the final intercom/audio/networking behavior. Several hardware and
protocol decisions have not been made yet; see
[`CONTEXT.md`](CONTEXT.md) for exactly what is implemented, what is a
stub, and what is still open.

## Target hardware

- **MCU:** ESP32-C3 (RISC-V, single core).
- **Dev board:** not finalized. `platformio.ini` currently targets
  `esp32-c3-devkitm-1` as a generic placeholder — update it once the real
  board/module is chosen.
- **Intercom interface circuit, audio hardware, door-release circuit:**
  not defined yet. The firmware isolates these behind interfaces (see
  `src/hardware/gpio.h`, `src/audio/audio.h`) so they can be implemented
  later without reworking the rest of the codebase.

## Stack

- [PlatformIO](https://platformio.org/) with the `espressif32` platform
  and Arduino framework.
- C++17.
- [Unity](http://www.throwtheswitch.org/unity) for unit tests, run on a
  `native` PlatformIO environment (host compiler, no ESP32 required) for
  all hardware-independent logic.

## Project layout

```text
src/         Firmware source (core, hardware, intercom, audio, network).
include/     Reserved for shared public headers (unused so far).
test/        Native unit tests (Unity), one PlatformIO test per directory.
docs/        Architecture documentation.
platformio.ini
CONTEXT.md   Operational memory of the project — read this first.
```

See [`docs/architecture.md`](docs/architecture.md) for the module map and
design rationale.

## Building

Requires [PlatformIO](https://platformio.org/install) (CLI or IDE
extension).

```bash
# Build the firmware for ESP32-C3
pio run -e esp32-c3

# Upload to a connected board
pio run -e esp32-c3 -t upload

# Open the serial monitor
pio device monitor -b 115200
```

## Running tests

```bash
# Run all native unit tests (state machine, events, line detector, protocol, audio)
pio test -e native
```

Tests only cover logic that does not require physical hardware. Anything
that depends on the real intercom line, GPIO wiring, or a Wi-Fi network
still needs manual validation on real hardware — see
`CONTEXT.md > Hardware Dependencies`.

## Where to look next

- **[`CONTEXT.md`](CONTEXT.md)** — current status, architectural
  decisions, open questions, technical debt, and next steps. This is the
  primary document for understanding the current state of the project;
  keep it updated whenever you change something relevant.
- **[`docs/architecture.md`](docs/architecture.md)** — module map, layering,
  and the reasoning behind the current boundaries.
