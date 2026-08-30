# InterBridge Firmware

Firmware for **InterBridge**, a bridge between a traditional analog
intercom and a mobile app, intended to eventually run on an **ESP32-C3**.

This repository currently contains an architectural foundation and a
AWS IoT Core integration layer with a real ESP32 MQTT/mTLS transport, while still
**not** being a validated end-to-end device. The control plane is designed
around:

- **MQTT 3.1.1 over TLS to AWS IoT Core**, with mutual TLS (a unique
  X.509 certificate per device) — see `src/network/mqtt_transport.h`.
- **BLE-first onboarding**: nearby BLE discovery is the primary way a
  user sets up a device; QR scanning and typing the 12-digit `setup_code`
  are fallback ways to find the *same* physical device, not separate
  flows — see `src/provisioning/provisioning_manager.h` and
  `docs/communication-protocol.md` section 7.
- **OTA firmware updates via AWS IoT Jobs** (not a custom application
  command) — see `src/ota/ota_manager.h` and `src/aws/jobs.h`.
- **A physical configuration/reset button** (short press: nothing; ~3s
  hold: (re)opens the 5-minute provisioning window; ~10s hold: factory
  reset) — see `src/hardware/button.h`. The button's GPIO is not
  assigned yet.

The isolated DEV smoke firmware has now been validated on a real generic
ESP32-C3 Super Mini against AWS IoT Core. Several production hardware, cloud,
and protocol decisions have not been made yet,
and several of the pieces above remain coordinators behind interfaces. The MQTT/mTLS implementation uses
`256dpi/MQTT` and `WiFiClientSecure`; production NVS, production SNTP and signing remain
incomplete. See [`CONTEXT.md`](CONTEXT.md) for exactly what is implemented,
what is a stub, and what is still open — and
[`docs/communication-protocol.md`](docs/communication-protocol.md) for
the full device/cloud protocol specification.

## Target hardware

- **MCU:** ESP32-C3 (RISC-V, single core).
- **Validated bench board:** generic 4 MB ESP32-C3 Super Mini using the
  compatible `esp32-c3-devkitm-1` definition and native USB CDC. This does not
  determine the module or behavior of the eventual custom PCB.
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
test/        Native unit tests (Unity), one PlatformIO test per directory (38 suites).
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
# Run all native unit tests (38 suites: state machine, events, intercom,
# MQTT topics, command parsing/handling/dedup, event outbox, reconnect
# backoff, button, device identity, persistent storage, Device Shadow,
# AWS IoT Jobs, OTA, health telemetry, provisioning (BLE-first onboarding
# state machine), BLE advertisement/security mode, status indicator,
# Fleet Provisioning, factory reset, MQTT transport, DEV MQTT smoke
# state, Si3050 bring-up controller, Si3050 ring detector, Si3050 clock
# probe math, DEV ring simulator button/event (Phase 3B.8), ...)
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

## Controlled DEV MQTT smoke firmware

A separate `esp32-c3-dev-mqtt` PlatformIO environment now provides a guarded,
non-actuating MQTT/mTLS bench harness. The earlier harness used a parallel
MQTT/TLS client and `DevMqttSmokeHandler`; it did not validate the production
classes added in PR #6. The corrected harness composes `Esp32AwsIotTransport`,
`RemoteCommandProcessor`, deduplication, and the fail-closed `CommandHandler`.
It does not replace production provisioning. See [docs/mqtt-dev-smoke-test.md](docs/mqtt-dev-smoke-test.md)
for scope, local placeholder configuration, safety constraints, and PC/ESP32
steps. The ordinary `esp32-c3` entry point remains `src/main.cpp` and requires no
DEV secrets header.

The first controlled device test validated build/upload, USB CDC, 2.4 GHz
Wi-Fi, MQTT/mTLS on port 8883 with an individual X.509 certificate, QoS 1
command subscription, QoS 0 health, safe `OPEN_DOOR` rejection and response,
and cold-boot reconnection. Transient DNS failures recovered through retry.
A later real-hardware run stayed online ~110 minutes, then lost the MQTT/TLS
session and DNS started failing (Wi-Fi itself stayed associated) - the
symptoms point at a local Wi-Fi/DNS-path issue, not AWS, though the exact
cause of that local degradation was not diagnosed. The DEV harness now
performs an explicit DNS preflight before every MQTT (re)connection attempt
and, after several consecutive DNS/TLS connectivity failures, a
conservative, cooldown-limited Wi-Fi interface recovery (never
`ESP.restart()`) - see [docs/mqtt-dev-smoke-test.md](docs/mqtt-dev-smoke-test.md)'s
"Real bench observation: MQTT/TLS lost after ~110 minutes online". This is
covered by native tests and compiles for both `esp32-c3`/`esp32-c3-dev-mqtt`,
but has **not** been re-validated on real hardware yet, and local Wi-Fi
reliability itself should not be considered fixed by this change alone.
Access-point loss and return while the board stays powered has **not** yet been
validated. A follow-up fix closed a `WiFi.disconnect()` async race in that
recovery cascade (the state machine now waits for a real disconnect signal
before re-associating, instead of possibly resuming DNS/MQTT over the stale
association) - see [docs/mqtt-dev-smoke-test.md](docs/mqtt-dev-smoke-test.md)'s
"Follow-up fix" subsection.

## DEV physical ring simulator (Phase 3B.8)

A separate `esp32-c3-dev-ring-simulator` PlatformIO environment lets a
momentary button (GPIO20, `INPUT_PULLUP`, wired to GND - see
[docs/dev-ring-simulator.md](docs/dev-ring-simulator.md)) publish a real
`RING_DETECTED` `DeviceEvent` through the exact same AWS IoT Basic Ingest
pipeline production uses, so the downstream notification pipeline can be
bench-tested without a real Si3050/intercom line. It reuses the DEV MQTT
smoke environment's connectivity state machine and the existing
`MqttTopics`/`Esp32AwsIotTransport`/`IEventOutbox` contract rather than
inventing a new one, and never touches the real Si3050 driver stack,
provisioning, BLE, or production Wi-Fi/AWS composition. Implemented and
compiled (native tests pass, all PlatformIO environments still build);
real-hardware boots have not yet associated to Wi-Fi, and a retest
revealed `esp32-c3-dev-mqtt` failing the exact same way on the same
session - both DEV mains share `DevMqttSmokeState`, which had a real
concurrent-retry defect (reissuing `WiFi.begin()` while a previous
association attempt was still outstanding). That coordinator is now fixed
to track an association attempt explicitly and resolve it via a real
Wi-Fi event or a separate timeout, but this has **not yet been confirmed
to make Wi-Fi associate** - the disconnect reason codes observed (2, 202)
still need a fresh hardware retest, and SSID/credential/network causes
are not ruled out. See
[docs/dev-ring-simulator.md](docs/dev-ring-simulator.md) for the wiring
diagram, GPIO rationale, manual test procedure, and the real bench
observations, and
[docs/roadmap-3b.md](docs/roadmap-3b.md) for how this fits with the rest
of the (cross-repo) ring-notification pipeline.

## Si3050/Si3011-19 firmware foundation (Phase 3A + 3B.2 PCM clock)

`src/intercom/si3050/` is a hardware-independent, natively-tested
foundation for the Si3050 DAA (+ Si3011/18/19 line-side device) that will
interface hardware Rev A to the analog intercom line. It models and
documents the Si3050's electrical bring-up sequence (`Si3050Controller`)
and a debounced `/RGDT` ring-line reader (`RingDetector`), gated behind
narrow SPI/PCM-clock/reset/delay interfaces. See [docs/si3050-bringup.md](docs/si3050-bringup.md)
for the full contract, its datasheet citations (`docs/Si3050-11-18-19.pdf`
is checked into this repository), the PCM clock's precise validation
status, and a bring-up checklist for when Rev A hardware exists.

**PCM clock generation is now real, integrated, and physically measured.**
The validation has three deliberately separate levels: (1) the isolated
Phase 3B.1 probe previously validated the exact `16 x 8` TDM geometry;
(2) the real `Esp32PcmClock` implementation in the normal `esp32-c3`
firmware has now also been reflashed and measured physically at approximately
`1.024 MHz` PCLK / `8 kHz` FSYNC / `128` clocks per frame; and (3) no real
Si3050 has been connected or initialized. Consequently, real SPI,
Si3050/DAA register access, PCM data (`DRX`/`DTX`), audio, ring, off-hook,
and relay behavior remain outside this validation and unvalidated. See
[docs/si3050-bringup.md](docs/si3050-bringup.md)'s "PCM clock: validation
status" for the precise distinction.

## Si3050 clock probe: ESP32-C3 -> ESP32 DevKitV1 (Phase 3B.1)

Two more isolated PlatformIO environments, `esp32-c3-si3050-clock-probe`
and `esp32dev-si3050-clock-meter`, form a bench experiment checking
whether an ESP32-C3 can generate a Si3050-compatible PCLK/FSYNC clock in
hardware (I2S TDM master mode) and whether a second, classic ESP32
DevKitV1 board can measure it by hardware pulse counting (PCNT). The
target is **PCM/SPI mode** (SPI for control, PCM for audio - the mode
InterBridge plans to use): `PCLK ~= 1.024 MHz`, `FSYNC = 8 kHz`,
ratio ~= 128 - not the `2.048 MHz`/256 figure required only by the
Si3050's separate GCI mode, which InterBridge does not use. See
[docs/si3050-clock-probe.md](docs/si3050-clock-probe.md)'s "Corrected
premise" section for the full datasheet-sourced distinction.

**Physically confirmed: the generator's 16 slots x 8 bits TDM geometry
reaches this target on real hardware.** The original 16 x 16 geometry
measured `pclk_hz ~= 1,024,000`/`fsync_hz ~= 16,000`/`ratio ~= 64` -
not the target. The generator was changed to request 16 x 8 (matching
the Si3050's own PCM/SPI PCM Highway description exactly - 128
requested clocks/frame instead of 256), and a real bench retest
confirmed `pclk_hz ~= 1,024,100`, `fsync_hz ~= 8,001`-`8,002`,
`ratio ~= 127.98`-`128.00` across stable reporting windows (the first
window after boot contained a brief startup transient and is excluded).
**This confirms the clock signal only** - no real Si3050 hardware has
been connected or initialized, and PCM audio data (`DRX`/`DTX`) and
audio content are untested. This validated *geometry* has since been
implemented for real in `Esp32PcmClock` and integrated into the normal
firmware's boot sequence (see "Si3050/Si3011-19 firmware foundation"
above) - but the standalone `esp32-c3-si3050-clock-probe` bench
environment itself remains a separate, isolated throwaway firmware, kept
as a bench regression check, not the production code path. See
[docs/si3050-clock-probe.md](docs/si3050-clock-probe.md) for the full
contract, wiring, build/flash/monitor commands, and the complete bench
record.

## Phase 2D MQTT command transport

`Esp32AwsIotTransport` now configures the established `256dpi/MQTT` client over
Arduino `WiFiClientSecure` for AWS IoT Data ATS on port 8883. It uses the provisioned
`device_id` unchanged as Client ID, explicit 30-second keepalive/1500 ms timeout, no
custom Last Will, and credentials obtained through `DeviceCredentialStore`. The loop
only attempts MQTT while Wi-Fi is connected, uses bounded full-jitter backoff, clears
subscription state on loss, and subscribes once after every connection. Commands are
accepted only from `interbridge/{device_id}/commands`, QoS 1, without wildcard; payloads
over 8 KiB are rejected before parsing. Responses use the existing Basic Ingest response
topic, QoS 1 and `retain=false`.

For a future local physical DEV check, generate the ignored
`include/interbridge_dev_secrets.h` from the example/generator and provide only local
values for `<WIFI_SSID>`, `<WIFI_PASSWORD>`,
`<AWS_IOT_DATA_ATS_ENDPOINT>`, `<DEVICE_ID>`, `<AMAZON_ROOT_CA_PEM>`,
`<DEVICE_CERTIFICATE_PEM>`, and `<DEVICE_PRIVATE_KEY_PEM>`. Never commit or log that
file. Missing/inconsistent endpoint, identity, CA, certificate, or key fails closed.

This change was tested only with host fakes and compile checks: Codex made no AWS call,
published no real MQTT message, performed no provisioning, and flashed no hardware. A
physical end-to-end validation remains required. `OPEN_DOOR` still emits only `ACCEPTED`
then `REJECTED/CAPABILITY_DISABLED`; it cannot invoke DTMF, relay, GPIO, key sequences,
or any physical opening. The app's visual command integration remains disabled until
API → AWS IoT → ESP32 → Basic Ingest → GET is validated. DTMF, relay, and opening
configuration, production NVS, provisioning, and onboarding remain deferred. Phase 2D
cannot be declared validated until the complete real-device path is tested.
