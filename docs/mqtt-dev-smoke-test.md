# DEV MQTT/mTLS smoke test

## Scope and safety

`esp32-c3-dev-mqtt` is a separate, guarded bench firmware for DEV AWS IoT
Core validation. It is **not production provisioning**. It connects Wi-Fi,
synchronizes UTC with NTP, opens MQTT 3.1.1/mTLS on port 8883, and reconnects
with capped exponential delay. MQTT connect, subscribe, poll, disconnect, and
publish all pass through `Esp32AwsIotTransport`. Commands pass through
`RemoteCommandProcessor`, existing deduplication, and `CommandHandler`; the door
capability and DEV hardware/system-control implementations are non-actuating.

PR #6 implemented and compiled `Esp32AwsIotTransport`, but the old physical harness
still instantiated its own MQTT/TLS stack and `DevMqttSmokeHandler`. This correction
removes that parallel path. BLE provisioning, production NVS credential persistence,
on-device private-key/CSR generation and Fleet Provisioning transports remain
stubs. Basic Ingest rules arrive in backend Phase 1E, so successful QoS
acknowledgment does not imply persistence. Phase 1D is complete only for the
first controlled DEV MQTT/mTLS device. This manual injection path is not a
production provisioning design.

## Backend contract

DEV uses only these explicit rule names:

- `interbridge_dev_ingest_rule`
- `interbridge_dev_response_rule`

For local `device_id`, the harness subscribes only to
`interbridge/{device_id}/commands` at QoS 1. It publishes responses only to
`$aws/rules/interbridge_dev_response_rule/interbridge/{device_id}/responses`.
Command responses use QoS 1. ClientId is exactly `device_id`. No mirror or diagnostic topic exists.
The harness does not access Shadow or Jobs.

The harness publishes the protocol `HealthReport` to the centralized
`MqttTopics::healthIngest()` Basic Ingest route at QoS 0 once after each successful
connection/subscription and every 60 seconds thereafter. The DEV backend uses
`FRESH_SECONDS=120`, so this cadence refreshes `last_seen_at` halfway through the
freshness window and tolerates one missed periodic report without changing the
backend threshold. A
failed attempt is not retried in the tight loop, preventing a reconnect/publish
storm: it waits until the next 60-second cadence. This produces up to 1,440 periodic
messages/device/day (plus reconnects), increasing DEV broker, rule, Lambda, and
storage traffic relative to an hourly report, but remaining four times lower than
reusing the 15-second local-status cadence (5,760 messages/device/day). Production
keeps its independently configurable cadence for explicit freshness/cost review;
the DEV override must not silently become the production default.
Only measurements available on the ESP32 are emitted: firmware version, safe idle
intercom state (the DEV hardware never changes it), wrap-safe uptime, RSSI, and free
heap, in addition to required protocol/device fields.

The maintained `256dpi/MQTT` client is used because it integrates with
`WiFiClientSecure`, supports MQTT 3.1.1, QoS 0 and QoS 1 publication and QoS 1 subscribe,
retained-message selection, clean sessions, and configurable keepalive/buffer.
The harness uses a buffer above the protocol's 8 KiB ceiling, keepalive 300,
clean session, no Last Will, and `retain=false` for every publish.

MQTT receive callbacks only validate and enqueue commands. The main loop drains
that bounded queue after `MQTTClient::loop()` returns, so command responses never
recursively publish while the MQTT/TLS client is dispatching an inbound packet.
TLS stream/handshake operations use the configured one-second timeout. A task
watchdog is fed only after the complete loop (including network work) returns.

The local heartbeat reports current state plus free/minimum heap and remaining
loop-task stack. Boot diagnostics report the previous reset reason, persistent
boot count, and only whether Wi-Fi configuration is present. Wi-Fi driver events
include disconnect reason codes. Each expired Wi-Fi attempt is explicitly torn
down before one new `WiFi.begin()`, with capped backoff and the next deadline
logged. Ten continuous minutes without full Wi-Fi/MQTT recovery triggers a
controlled restart as a last resort; a call stuck inside the network stack is
recovered sooner by the watchdog.

## Local credentials and build

1. Keep `endpoint.txt`, `AmazonRootCA1.pem`, `device-certificate.pem.crt`,
   and `private.pem.key` in an explicitly selected directory outside this repo.
2. Run `scripts/generate_dev_secrets_header.ps1 -CredentialsDirectory
   <external-folder> -DeviceId ib-<32-lowercase-hex>`. It prompts for SSID and
   a secure-string password, validates endpoint/Git ignore rules, and emits
   escaped one-line C++ macros without displaying credential values. Never use
   a multiline raw string inside a `#define`.
3. Run `pio run -e esp32-c3-dev-mqtt` and flash that environment explicitly.
4. Attach the serial monitor. Logs contain operation status and credential
   presence only, never secret values or command payloads. The 15-second
   `local_status` line remains local and is separate from `health publish: ok/failed`.
   Temporary Phase 2D diagnostics log command receipt, safe rejection codes and
   publish stages. For a valid timestamp they log only `age_s` and `remaining_s`,
   never absolute timestamps or complete identifiers; remove these two differences
   after the DEV clock investigation is closed.
5. Send a protocol-v1 command on that exact device's commands topic and verify a
   protocol-v1 `ACCEPTED` followed by `REJECTED/CAPABILITY_DISABLED`. Verify no
   `COMPLETED`, `DOOR_OPENED`, DTMF, GPIO, relay, pulse, key, restart, or remote
   provisioning action. Also exercise invalid clock, expiry, oversize, wrong-topic,
   duplicate, publish-failure, disconnect/reconnect, and one resubscribe per connection.
6. In a future bench session, interrupt and restore the access point while the
   board remains powered; that specific recovery scenario is not yet validated.

The local header is ignored by Git. Selecting this environment without it fails
at preprocessing with a clear error. The default `esp32-c3` environment neither
includes nor requires it. Never reuse this manual injection path for production:
production remains on-device key generation, CSR, Fleet Provisioning by Trusted
User, and a permanent private key that never leaves the device.

## Continuous integration

GitHub Actions runs repository credential safety checks, all native tests, the
ordinary `esp32-c3` build, and a compile-only `esp32-c3-dev-mqtt` build using
PlatformIO 6.1.18 on Python 3.12. CI copies the committed example header to the
ignored local-header path immediately before the smoke build. Those values stay
obvious placeholders: firmware is compiled but never executed, no Wi-Fi or AWS
connection is attempted, no hardware is flashed, and no GitHub secrets are
used.

## Real bench result

The result below is historical: it exercised the removed parallel handler, not the
PR #6 transport/processor composition. Codex made no AWS call, published no real MQTT
message, and flashed no board for this correction. The corrected harness still requires
a complete physical validation whose expected result is ordered `ACCEPTED` then
`REJECTED/CAPABILITY_DISABLED` through Basic Ingest.

A generic 4 MB ESP32-C3 Super Mini (PlatformIO 6.1.18, temporary compatible
`esp32-c3-devkitm-1` definition) validated build/upload, native USB CDC with
`ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1`, 2.4 GHz Wi-Fi, MQTT/mTLS
8883 with a unique device certificate, the existing ClientId/topics/QoS/retain
contract, safe `OPEN_DOOR` rejection and response, cold-boot reconnection, and
recovery from transient DNS failures. DHCP-provided DNS remains authoritative.

Not validated: access-point loss/return while powered, real BLE onboarding,
NVS, Fleet Provisioning, Secure Boot, Flash Encryption, intercom hardware,
Phase 1E Basic Ingest persistence, or the final custom PCB. A bounded serial
wait preserves headless operation; a credential-free 15-second heartbeat helps
late-attached monitors without logging SSID, endpoint, identity, PEM, or payload.

## Phase 2D clock correction

The earlier harness treated any `time()` value above a constant as synchronized.
That is unsafe because an RTC value can look plausible while a new SNTP attempt is
still pending. The DEV clock now becomes trustworthy only after the ESP SNTP
completion callback, a short transition/settling gate, and confirmation that SNTP
is not in progress. `CommandHandler` continues to compare signed 64-bit Unix epoch
**seconds** directly: it never uses `millis()`, timezone conversion, milliseconds,
or signed 32-bit storage. Expiry remains strict (`now > expires_at`) with no grace
period; the existing five-second allowance applies only to a slightly future
`issued_at`.
