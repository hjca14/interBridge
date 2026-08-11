# InterBridge Firmware

Firmware for **InterBridge**, a bridge between a traditional analog
intercom and a mobile app, intended to eventually run on an **ESP32-C3**.

This repository currently contains an architectural foundation and a
first AWS IoT Core integration layer — clean, testable, extensible, but
**not** a working end-to-end device yet. The control plane is designed
around:

- **MQTT 3.1.1 over TLS to AWS IoT Core**, with mutual TLS (a unique
  X.509 certificate per device) — see `src/network/mqtt_transport.h`.
- **BLE-based Wi-Fi provisioning** — see `src/provisioning/ble_provisioning.h`.
- **OTA firmware updates via AWS IoT Jobs** (not a custom application
  command) — see `src/ota/ota_manager.h` and `src/aws/jobs.h`.
- **A physical configuration/reset button** (short press: nothing; ~3s
  hold: enter provisioning; ~10s hold: factory reset) — see
  `src/hardware/button.h`. The button's GPIO is not assigned yet.

Several hardware, cloud, and protocol decisions have not been made yet,
and several of the pieces above are real, tested coordinators sitting
behind interfaces whose ESP32/AWS-side implementation is still a
documented stub (no MQTT/TLS client, no NTP, no real NVS, no signing
scheme). See [`CONTEXT.md`](CONTEXT.md) for exactly what is implemented,
what is a stub, and what is still open — and
[`docs/communication-protocol.md`](docs/communication-protocol.md) for
the full device/cloud protocol specification.

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
src/         Firmware source: core, hardware, intercom, audio, storage,
             provisioning, network, protocol, aws, ota.
include/     Reserved for shared public headers (unused so far).
test/        Native unit tests (Unity), one PlatformIO test per directory (24 suites).
docs/        Architecture documentation and the communication protocol spec.
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
# Run all native unit tests (24 suites: state machine, events, intercom,
# MQTT topics, command parsing/handling/dedup, event outbox, reconnect
# backoff, button, device identity, persistent storage, Device Shadow,
# AWS IoT Jobs, OTA, health telemetry, provisioning, Fleet Provisioning,
# factory reset, MQTT transport, ...)
pio test -e native
```

Tests only cover logic that does not require physical hardware, a real
Wi-Fi/AWS IoT connection, or real cryptography. Anything that depends on
the real intercom line, GPIO wiring, Wi-Fi, BLE, NTP, or AWS IoT Core
still needs manual validation on real hardware — see
`CONTEXT.md > Hardware Dependencies`.

## Where to look next

- **[`CONTEXT.md`](CONTEXT.md)** — current status, architectural
  decisions, open questions, technical debt, and next steps. This is the
  primary document for understanding the current state of the project;
  keep it updated whenever you change something relevant.
- **[`docs/architecture.md`](docs/architecture.md)** — module map, layering,
  and the reasoning behind the current boundaries.
- **[`docs/communication-protocol.md`](docs/communication-protocol.md)** —
  the authoritative device/cloud protocol specification (topics, message
  shapes, command lifecycle, error codes, provisioning, OTA).
