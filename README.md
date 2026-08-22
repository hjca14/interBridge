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
test/        Native unit tests (Unity), one PlatformIO test per directory (26 suites).
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
# Run all native unit tests (26 suites: state machine, events, intercom,
# MQTT topics, command parsing/handling/dedup, event outbox, reconnect
# backoff, button, device identity, persistent storage, Device Shadow,
# AWS IoT Jobs, OTA, health telemetry, provisioning (BLE-first onboarding
# state machine), BLE advertisement/security mode, status indicator,
# Fleet Provisioning, factory reset, MQTT transport, ...)
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
non-actuating MQTT/mTLS bench harness. It does not replace production transport
or provisioning. See [docs/mqtt-dev-smoke-test.md](docs/mqtt-dev-smoke-test.md)
for scope, local placeholder configuration, safety constraints, and PC/ESP32
steps. The ordinary `esp32-c3` entry point remains `src/main.cpp` and requires no
DEV secrets header.

The first controlled device test validated build/upload, USB CDC, 2.4 GHz
Wi-Fi, MQTT/mTLS on port 8883 with an individual X.509 certificate, QoS 1
command subscription, QoS 0 health, safe `OPEN_DOOR` rejection and response,
and cold-boot reconnection. Transient DNS failures recovered through retry.
Access-point loss and return while the board stays powered has **not** yet been
validated.


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
configuration remain deferred. Phase 2D is therefore not declared complete.
