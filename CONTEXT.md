# InterBridge Firmware Context

This file is the operational memory of this project. Read it before making
changes. **Every relevant implementation/change must update this file in
the same task** — do not treat documentation as separate work.

## Project Overview

InterBridge is firmware that bridges a traditional analog intercom with a
mobile app, intended to eventually run on an **ESP32-C3**. It detects
ring/off-hook/on-hook on the physical intercom line, coordinates audio
between the intercom and the app (not yet designed), and communicates
with **AWS IoT Core** over MQTT/TLS (mutual X.509 authentication) to
allow remote command handling (door open, restart), telemetry (health,
Device Shadow), OTA updates (AWS IoT Jobs), and BLE-based Wi-Fi
provisioning.

## Current Status

**Architectural foundation + a first, honestly-scoped AWS IoT Core
integration layer.** Two implementation passes have happened:

1. The initial skeleton (state machine, events, logger, hardware/
   intercom/audio abstractions) — hardware-independent, unit tested.
2. This pass: device identity, persistent storage, MQTT topic building,
   protocol message models, command handling (time-safety + duplicate
   protection), event outbox, health telemetry, Device Shadow, AWS IoT
   Jobs/OTA, BLE provisioning, AWS IoT Fleet Provisioning (CSR flow), the
   physical config/reset button, and factory reset — all built against
   interfaces, with real logic where it doesn't require unavailable
   hardware/AWS/crypto, and clearly-labeled stubs where it does.

**Nothing in this codebase can complete a real AWS IoT connection, a real
BLE provisioning session, or a real signed OTA update yet.** See
Hardware Dependencies and Open Questions.

**Important:** `docs/communication-protocol.md` was substantially rewritten
by the user (to v1.1) partway through this pass, after the code below had
already been implemented against an earlier reading of the AWS
architecture. The two are **not** fully reconciled — see
[Decisions > Protocol doc v1.1 reconciliation](#decision-protocol-doc-v11-reconciliation-not-yet-complete)
below for exactly what matches, what's missing, and what should be
revisited before relying on this code against that document.

## Architecture

See [`docs/architecture.md`](docs/architecture.md) for the full module
map. Summary of what exists now, by directory:

- `core/` — events, state machine, logger, version, `random_id.h` (secure
  128-bit ID generation). No Arduino dependency.
- `hardware/` — `gpio.h` (intercom line/door output), `clock.h`
  (monotonic + wall-clock time abstraction), `button.h` (config/reset
  button, debounced), `system_control.h` (restart).
- `intercom/` — line detection, `Intercom` facade (door output now
  reports real success/failure, see Decisions).
- `audio/` — unchanged stub (`NullAudioIO`).
- `network/` — `wifi.h` (now behind an `IWifiConnection` interface),
  `mqtt_topics.h`, `mqtt_transport.h` (`IDeviceTransport`),
  `reconnect_manager.h`, `health_reporter.h`. `protocol.h`
  (`ICommunicationProtocol`/`NullProtocol`) is unchanged but now
  superseded/unused — see Decisions.
- `protocol/` (new) — `messages.h` (protocol v1 message models + JSON,
  via ArduinoJson), `command_cache.h` (duplicate protection),
  `command_handler.h` (time-safety + dispatch), `event_outbox.h`
  (offline-safe event queue).
- `storage/` (new) — `persistent_store.h` (`IPersistentStore`),
  `memory_store.h` (native), `nvs_store.h` (ESP32, **stub**),
  `credential_store.h` (isolates certificate/private-key access).
- `provisioning/` (new) — `device_identity.h`, `ble_provisioning.h`
  (**stub** - framework limitation, see Decisions), `provisioning_manager.h`
  (converges triggers), `fleet_provisioning.h` (CSR flow),
  `factory_reset_coordinator.h`.
- `aws/` (new) — `device_shadow.h` (reported/desired), `jobs.h`
  (`IJobsClient`, `JobsCoordinator`).
- `ota/` (new) — `firmware_validation.h` (`IFirmwareVerifier` - signature
  check **fails closed, not implemented**), `ota_manager.h`
  (`OtaCoordinator`, `IOtaPlatform`).
- `main.cpp` — composition root; grew substantially (many objects to
  wire) but contains no branching business logic itself - see Decisions.

Two new top-level directories (`storage/`, not originally sketched, and
reusing `provisioning/`/`aws/`/`ota/` per the task's suggested layout)
were added; this matches "you may choose better names if they improve
clarity" from the original setup task.

## Implemented

*(Distinguishing Implemented / Stub / Provisional / Not implemented per
module - see also the Tests section for exactly what was verified.)*

- **Core**: `core/random_id.h` - `IRandomSource` + `Esp32RandomSource`
  (hardware RNG, **needs on-hardware validation**) + `FakeRandomSource`;
  `generateHexId()` for `evt-`/`cmd-`/`ib-`-prefixed 128-bit IDs.
- **Clock**: `hardware/clock.h` - `IClock`/`Esp32Clock`/`FakeClock`.
  `monotonicMs()` implementable via `millis()`. `hasValidTime()`/
  `unixTimeSeconds()` are **stubs that always report no valid time** - no
  NTP/time sync exists. This is a deliberate fail-safe default, not an
  oversight (see Command Time Safety below).
- **Button**: `hardware/button.h` - `ButtonController` fully implemented
  and unit tested (debounce, one-shot 3s/10s thresholds, no repeat while
  held). `Esp32ButtonInput` is a stub (no GPIO assigned).
- **System control**: `hardware/system_control.h` - `Esp32SystemControl`
  calls `ESP.restart()` (**needs on-hardware validation**;
  `FakeSystemControl` used in tests).
- **Door output honesty**: `IHardwareIO::setDoorOutput()` and
  `Intercom::requestDoorOpen()` now return `bool` instead of `void` -
  see Decisions.
- **Persistent storage**: `storage/persistent_store.h` interface,
  `MemoryStore` (**implemented**, used by every native test),
  `NvsStore` (**stub** - no real NVS/Preferences wiring), `DeviceCredentialStore`
  (**implemented** over whatever `IPersistentStore` it's given).
- **Device identity**: `provisioning/device_identity.h` -
  `isValidDeviceId()` + `DeviceIdentityProvider` (**implemented**: loads a
  stored id or generates+persists one via `IRandomSource`, format
  `ib-<32 hex>`).
- **MQTT topics**: `network/mqtt_topics.h` - `MqttTopics` (**implemented**
  and unit tested: commands, Basic Ingest events/health/responses, Device
  Shadow, Jobs, Fleet Provisioning topics).
- **Reconnect backoff**: `network/reconnect_manager.h` - `ReconnectManager`
  (**implemented**: exponential + full jitter, 1s-300s, unit tested with a
  fake random source; `main.cpp` drives it non-blockingly against
  `IClock::monotonicMs()`).
- **MQTT transport**: `network/mqtt_transport.h` - `IDeviceTransport`,
  `FakeDeviceTransport` (**implemented**, used in tests), `Esp32AwsIotTransport`
  (**stub** - no MQTT/TLS client library chosen/wired yet).
- **Protocol messages**: `protocol/messages.h` - `DeviceEvent`,
  `HealthReport`, `CommandResponse`, `DeviceCommand`+`parseCommand()`
  (**implemented** via ArduinoJson: 8 KiB size limit, protocol_version
  check, required-field validation, `command_id` format validation
  (`isValidCommandId()`, 32 lowercase hex chars), unknown-command
  tolerance, unknown-JSON-field tolerance). `ProtocolIntercomState`
  (**implemented**: closed `IDLE`/`RINGING`/`OFF_HOOK`/`IN_CALL`/`ERROR`
  vocabulary) added in the 2026-08-11 protocol reconciliation pass - see
  Decisions.
- **Command handling**: `protocol/command_handler.h` - `CommandHandler`
  (**implemented**: time-safety validation against `issued_at`/
  `expires_at`, dispatch for `OPEN_DOOR`/`RESTART`, structural rejection
  of `ENTER_PROVISIONING`/`FACTORY_RESET`/reserved call commands/unknown
  commands). Synchronous simplification - see Decisions.
- **Duplicate protection**: `protocol/command_cache.h` -
  `InMemoryDedupCache` (**implemented**) and `PersistentDedupCache`
  (**implemented architecture**, but doesn't survive reboot on real
  hardware yet because `NvsStore` is a stub).
- **Event outbox**: `protocol/event_outbox.h` - `MemoryEventOutbox`/
  `PersistentEventOutbox` (**implemented**: bounded FIFO, `event_id`
  preserved across reload; persistence has the same `NvsStore` caveat).
- **Health telemetry**: `network/health_reporter.h` - `HealthReporter`
  (**implemented**: cadence + force-publish, unit tested). `wifi_rssi`/
  `free_heap` in the actual `HealthReport` sent by `main.cpp` are
  currently hardcoded to `0` (**not wired to a real reading**).
- **Device Shadow**: `aws/device_shadow.h` - `DeviceShadow`
  (**implemented**: builds `reported`, parses `health_interval_s` from
  `/delta`, ignores unknown fields).
- **AWS IoT Jobs / OTA**: `aws/jobs.h` (`JobsCoordinator`, **implemented**
  against `IJobsClient`; `Esp32JobsClient` is a **stub**) and
  `ota/ota_manager.h` (`OtaCoordinator`, **implemented** against
  `IOtaPlatform`/`IFirmwareVerifier`; `Esp32OtaPlatform` is a **stub**,
  `DefaultFirmwareVerifier::verifySignature()` **always returns false -
  fails closed by design**, since no signing scheme exists).
- **BLE provisioning**: `provisioning/ble_provisioning.h` -
  `Esp32BleProvisioning` is a **stub**, and deliberately documented as
  such: the real design intent is ESP-IDF Unified Provisioning, which
  isn't exposed by the `framework = arduino` this project currently
  targets. `FakeBleProvisioning` is fully implemented for tests.
- **Provisioning orchestration**: `provisioning/provisioning_manager.h` -
  `ProvisioningManager` (**implemented** and unit tested: converges
  "missing Wi-Fi at boot" and "physical button" triggers, drives BLE
  advertise → credentials received → Wi-Fi connect → completed).
- **Fleet Provisioning (CSR flow)**: `provisioning/fleet_provisioning.h` -
  `FleetProvisioningCoordinator` (**implemented** and unit tested against
  fakes). `Esp32KeyPairGenerator`/`Esp32FleetProvisioningTransport` are
  **stubs** (no crypto library, no AWS account/template). **Not yet
  invoked from `main.cpp`'s loop** - see Technical Debt.
- **Factory reset**: `provisioning/factory_reset_coordinator.h` -
  `FactoryResetCoordinator` (**implemented** and unit tested: clears
  Wi-Fi credentials + provisioned flag, preserves device_id and AWS
  credentials).
- **`main.cpp`**: rewired to wire all of the above together; still no
  business logic in `setup()`/`loop()` themselves (delegated to small
  `updateX()` helper functions that call into coordinators).
- **Tests**: 24 native test suites (previously 5), ~130 assertions total,
  all passing under the validation method described in Tests below.

## Decisions

*(New decisions from this pass. Earlier decisions from the initial
skeleton - state machine event-driven design, `IHardwareIO`'s narrow
interface, ring detection not implemented, Wi-Fi allowed to depend on
Arduino directly, `esp32-c3-devkitm-1` placeholder board - are preserved
below, unchanged, for history.)*

### Decision: AWS IoT Core is the control-plane cloud, MQTT 3.1.1 over mTLS
Motivo: explicit architecture decision given for this task (previously
open). This moved several previously-open items to decided:
application control protocol = MQTT/TLS to AWS IoT Core; device auth =
unique X.509 certificate per device via mTLS; Wi-Fi provisioning = BLE
(already decided in the prior pass, now with an AWS Fleet Provisioning
follow-on); OTA = AWS IoT Jobs (not a custom `UPDATE_FIRMWARE`
application command); cloud/backend = primary remote control plane;
local firmware = remains operational independently (intercom polling
never blocks on network state); physical config/reset button = required,
GPIO still open.
Consequência: `network/protocol.h`'s `ICommunicationProtocol`/
`NullProtocol` (added in the previous pass as a transport-agnostic
placeholder) is now superseded by the concrete AWS-oriented stack
(`mqtt_transport.h`, `mqtt_topics.h`, `protocol/messages.h`,
`command_handler.h`). It was **not deleted** (still compiles, still
tested) because removing working code wasn't necessary and it costs
nothing to leave as historical/potentially-reusable scaffolding, but
`main.cpp` no longer constructs it. Treat it as legacy.

### Decision: command time-safety uses Unix seconds, not ISO-8601, for issued_at/expires_at
Motivo: `DeviceCommand.issuedAtUnixSeconds`/`expiresAtUnixSeconds` are
compared directly against `IClock::unixTimeSeconds()` every time a
command is handled; parsing ISO-8601 on-device for that comparison would
require a real date/time parser for no benefit, since these fields are
machine-consumed, not human-facing (unlike event `timestamp`, which stays
ISO-8601 per the original design).
Consequência: `protocol/messages.h`'s `parseCommand()` expects
`issued_at`/`expires_at` as JSON integers, and rejects a string in either
field the same way it would reject a missing field (`hasIssuedAt`/
`hasExpiresAt` stay `false`, which fails time-safety validation with
`INVALID_TIMESTAMP` - see `test_command_parser`'s
`test_iso8601_string_timestamp_is_not_accepted_as_a_command_timestamp`).
**Resolved 2026-08-11 (protocol reconciliation pass):**
`docs/communication-protocol.md` (now v1.2) was updated to match the
code exactly - section 18's example and section 14.1's new "Timestamp
representations differ by message type" note both now show Unix epoch
seconds for `issued_at`/`expires_at`, explicitly distinct from
`DeviceEvent.timestamp`'s ISO-8601 format. No code change was needed;
only the doc was wrong.

### Decision: command handling is synchronous; no intermediate ACCEPTED response yet
Motivo: `Intercom::requestDoorOpen()` and `ISystemControl::restart()` are
both currently synchronous (and, for the door, backed by a stub that
can't do anything asynchronous yet anyway). Implementing a real two-phase
ACCEPTED-then-terminal-response lifecycle for a synchronous stub would be
premature.
Consequência: `CommandHandler::handle()` returns one terminal response
(`COMPLETED`/`FAILED`/`REJECTED`) for `OPEN_DOOR`, and one `ACCEPTED`
response for `RESTART` (which has no same-call terminal state to report).
Revisit once door actuation is genuinely asynchronous - see Technical
Debt.

### Decision: `IHardwareIO::setDoorOutput()` and `Intercom::requestDoorOpen()` now return `bool`
Motivo: `CommandHandler` must not report `OPEN_DOOR` as `COMPLETED`
unless the hardware layer genuinely reports success (explicit
requirement of this task: "do not fake real-world success"). The
previous `void` signature made that impossible to express honestly.
Consequência: `Esp32GpioHardware::setDoorOutput()` now explicitly returns
`false` (it does nothing real). Every real `OPEN_DOOR` attempt on current
hardware honestly resolves to `FAILED`/`DOOR_OUTPUT_FAILURE`, not a fake
success. `test_line_detector`'s `MockHardware` and its tests were updated
for the new signature; two new tests were added for
`Intercom::requestDoorOpen()`'s forwarding behavior (previously
untested).

### Decision: `WifiManager` is now accessed through an `IWifiConnection` interface
Motivo: `ProvisioningManager` needs to drive Wi-Fi connection state
(`begin()`/`update()`/`isConnected()`) and be unit-testable natively.
`WifiManager`'s `.cpp` is excluded from the native build (it depends on
Arduino `WiFi.h`), so a concrete `WifiManager&` dependency would make
`ProvisioningManager` untestable natively (link error: the real methods
aren't compiled in that environment).
Consequência: added `IWifiConnection` (in `network/wifi.h`); `WifiManager`
implements it for real, `FakeWifiConnection` implements it for tests.
`FakeWifiConnection`'s implementation lives in a **new file**,
`network/wifi_fake.cpp`, specifically because it has to be compiled
natively while `wifi.cpp` (the real, Arduino-dependent implementation)
stays excluded - splitting them was required, not just tidiness.

### Decision: `WIFI_CONNECTED`/`WIFI_DISCONNECTED` are not part of the protocol event vocabulary
Motivo: this project's AWS IoT Core direction relies on AWS's own
connectivity lifecycle events as authoritative for online/offline state
(not a custom device-published signal), and Wi-Fi RSSI is already carried
through Device Shadow/health reports. Publishing a redundant custom event
for the same information seemed like unnecessary surface area.
Consequência: `core/events.h`'s internal `EventType::WifiConnected`/
`WifiDisconnected` still exist and still drive the local `StateMachine`
(unchanged from the prior pass), but `main.cpp`'s `toProtocolEvent()`
deliberately maps them to `std::nullopt` - they are never published as
MQTT events. If a future requirement needs an explicit Wi-Fi connectivity
event over MQTT, this is the one mapping to revisit.

### Decision: protocol reconciliation pass (2026-08-11) — error codes, command_id format, intercom_state, QR format
**What happened:** a follow-up task explicitly asked to make the
firmware and `docs/communication-protocol.md` (which the user had
rewritten to v1.1 mid-implementation, as noted in the entry this one
replaces) into one consistent source of truth for protocol v1, without
touching AWS IoT Core/MQTT/NTP/BLE/NVS/Lambda/API Gateway/Cognito
implementation. This is now **resolved** for the specific items listed
below; broader backend/infrastructure topics (claim flow implementation,
environment separation, Cognito/API Gateway/Lambda) remain intentionally
out of scope for firmware - see Open Questions.

**Error codes — resolved, doc and code now match exactly.**
`ProtocolErrorCode` (`protocol/messages.h`) gained `NotProvisioned`,
`WifiUnavailable`, `CloudUnavailable` (`NOT_PROVISIONED`,
`WIFI_UNAVAILABLE`, `CLOUD_UNAVAILABLE` on the wire) to match
`docs/communication-protocol.md` section 21's canonical list, which now
also documents the extra time-safety codes the firmware already had
(`PAYLOAD_TOO_LARGE`, `COMMAND_EXPIRED`, `CLOCK_NOT_TRUSTWORTHY`,
`INVALID_TIMESTAMP`, `PROVISIONING_FAILED`) rather than firmware losing
them. Every code's *origin* (device/backend/application) is now
documented in both places: a per-enumerator comment in `messages.h` and
a table in section 21. `NOT_PROVISIONED`/`WIFI_UNAVAILABLE`/
`CLOUD_UNAVAILABLE` are explicitly marked backend/application-origin -
**nothing in the firmware constructs a `ProtocolError` with these three
codes**, and that must stay true; they exist in the shared enum only so
the wire contract is complete for consumers, not so the device can
fabricate conditions it can't actually detect.

### Decision: command_id must be exactly 32 lowercase hex characters, no prefix
Motivo: the reconciliation task's canonical command JSON example uses a
bare 32-hex `command_id` (`"0123456789abcdef0123456789abcdef"`), with no
`cmd-` prefix - unlike `event_id` (`evt-` prefix) and `device_id` (`ib-`
prefix), both of which are firmware-generated via `core/random_id.h`.
`command_id` is always backend-generated, so this asymmetry is
deliberate and now documented in `docs/communication-protocol.md`
section 14.
Consequência: added `isValidCommandId()` (`protocol/messages.h/.cpp`);
`parseCommand()` now rejects (`INVALID_PAYLOAD`) any `command_id` that
isn't exactly `^[0-9a-f]{32}$`, replacing the previous "just check it's
non-empty" logic. **Only `test_command_parser` needed updating** - it's
the only test file that calls `parseCommand()` with a JSON string;
`test_command_handler` and `test_command_cache` construct `DeviceCommand`/
dedup-cache entries directly and treat `commandId` as an opaque string,
so they're unaffected by this stricter parse-time check.

### Decision: `ProtocolIntercomState` — a closed, protocol-only enum separate from `core::State`
Motivo: `HealthReport.intercomState`/`ShadowReportedState.intercomState`
were previously populated via `core::State`'s `toString()`
(`main.cpp:277`, prior to this pass), which can produce `"BOOT"` - not
one of the reconciliation task's canonical values (`IDLE`, `RINGING`,
`OFF_HOOK`, `IN_CALL`, `ERROR`). Reusing the internal diagnostic string
as the wire contract was the root problem, the same class of issue the
`ProtocolEventName`/`core::EventType` split already solved for events.
Consequência: added `ProtocolIntercomState` + `toString()` to
`protocol/messages.h/.cpp`, and `toProtocolIntercomState(core::State)` in
`main.cpp` (same pattern/location as `toProtocolEvent()`).
`State::Boot` maps defensively to `Idle` (documented as unreachable in
practice, since `finishBoot()` always runs before `loop()` - and
therefore before anything that publishes telemetry - ever executes).
**`OFF_HOOK` is defined but not reachable**: `core::StateMachine`
transitions `Ringing` → `InCall` directly on the `OffHook` event; there
is no resting "off-hook but not yet in a call" state to map it from.
This is documented in `messages.h`, `main.cpp`, and
`docs/communication-protocol.md` section 22.1 as intentional - the
alternative (inventing a state the firmware doesn't actually have) would
violate "não simule transições de hardware."

### Decision: product-ownership QR code format is now specified (doc only)
Motivo: `docs/communication-protocol.md` previously said the QR "contains
at minimum: device_id, claim_code" without a concrete, parseable format.
Consequência: section 4.1 now specifies the canonical
`interbridge://claim?v=1&device_id=ib-<32 hex>&claim_code=<secret>` URI
with explicit validation rules (scheme/host/`v`/`device_id` regex/
non-empty `claim_code`/duplicate-param rejection/percent-decoding/never
logging `claim_code`). **This is documentation only** - no QR
scanning/parsing code was added to the firmware, per the task's explicit
instruction that the firmware doesn't need one yet. `DeviceIdentity`
(`provisioning/device_identity.h`) still has no `claim_code` field -
that remains open, see Open Questions.

**Still genuinely open after this pass** (unchanged, backend/
infrastructure work outside firmware scope for now): `claim_code`
representation in `DeviceIdentity`/BLE provisioning payload; environment
separation (DEV/PROD) in `AwsIotConnectionConfig`; Cognito/API Gateway/
Lambda application backend; ownership transfer/decommissioning; AWS
account/region/rule names/Fleet Provisioning template. None of these
were in scope for this reconciliation pass (which was explicitly
protocol-consistency-only, not infrastructure), and none were touched.

## Known Limitations

*(From the initial skeleton, still true: `Esp32GpioHardware` is a stub,
ring detection isn't implemented, door actuation has no pulse timing,
Wi-Fi credentials aren't provisioned via `main.cpp` directly anymore -
now via `ProvisioningManager` - and this environment has no PlatformIO/
gcc. New from this pass:)*

- `NvsStore` (`storage/nvs_store.h`) is a complete stub - **nothing
  persists across reboot on real hardware yet**, even though
  `DeviceCredentialStore`, `PersistentDedupCache`, `PersistentEventOutbox`,
  and `DeviceIdentityProvider` are all written against the
  persistence-ready `IPersistentStore` interface. All of them work
  correctly today against `MemoryStore` (proven by 24 native test
  suites); only the real ESP32 backend is missing.
- `Esp32AwsIotTransport` (`network/mqtt_transport.h`) cannot connect to
  anything - no MQTT 3.1.1/TLS client library has been chosen or wired
  up. `main.cpp`'s entire network stack (commands, events, health,
  Shadow, Jobs) is therefore inert on real hardware today; it only runs
  meaningfully against `FakeDeviceTransport` in tests.
- `hardware/clock.h`'s `Esp32Clock::hasValidTime()` always returns
  `false` (no NTP/time sync) - this means, as designed,
  **no remote command can currently be accepted on real hardware**
  (`CLOCK_NOT_TRUSTWORTHY`). This is intentional fail-safe behavior, not
  an oversight, but it does mean the whole command pipeline is
  unreachable end-to-end until NTP exists.
- `Esp32BleProvisioning` is a stub for a structural reason, not a missed
  task: ESP-IDF Unified Provisioning (the intended design) isn't exposed
  by `framework = arduino`. See Decisions in the prior pass's CONTEXT
  entry and `docs/communication-protocol.md` section 7/20.
- `DefaultFirmwareVerifier::verifySignature()` always returns `false` -
  no signing scheme/public key chosen. Real OTA cannot complete
  end-to-end even once download/transport exist, until this changes.
- `Esp32KeyPairGenerator`/`Esp32FleetProvisioningTransport` are stubs -
  no on-device crypto library for keypair/CSR generation, no AWS account/
  template. `FleetProvisioningCoordinator` itself is fully implemented
  and tested against fakes, but isn't invoked anywhere in `main.cpp`'s
  loop yet (see Technical Debt).
- `network/health_reporter.h`'s cadence logic is solid, but the actual
  `HealthReport` built in `main.cpp` hardcodes `wifiRssi = 0` and
  `freeHeapBytes = 0` - not wired to real readings.
- This environment (the sandbox this pass was implemented in) has no
  PlatformIO, gcc, or clang, and no internet-connected `pio` CLI was
  used - see Tests for exactly what substituted for `pio run`/`pio test`.

## Open Questions

*(Superset of the initial pass's list - board/GPIOs/intercom electrical
interface/audio hardware/audio codec/OTA artifact hosting remain exactly
as open as before. New/updated from this pass - see also
`docs/communication-protocol.md` v1.1 section 37 for the user's own,
more complete "Still Open" list, which is the more authoritative version
of this section going forward.)*

- MQTT 3.1.1/TLS client library choice for ESP32 (needed for
  `Esp32AwsIotTransport`).
- NTP/time-sync implementation (needed before `Esp32Clock::hasValidTime()`
  can ever return `true`, which gates all remote command handling).
- On-device crypto library choice (needed for `Esp32KeyPairGenerator`,
  and for `DefaultFirmwareVerifier`'s signature verification once a
  signing scheme is chosen).
- Firmware signing scheme and public key distribution.
- AWS account/region, Fleet Provisioning template name, Basic Ingest rule
  names (`ingestRuleName`/`responseRuleName` in `MqttTopicsConfig`
  currently default to development placeholders).
- Real BLE service/characteristic UUIDs, and whether to switch
  `platformio.ini`'s `esp32-c3` environment to `framework = espidf` (or a
  mixed framework) to get ESP-IDF Unified Provisioning for real.
- Proof-of-Possession generation/persistence strategy (currently
  regenerated every boot in `main.cpp`, not stored).
- ESP32 OTA partition/library approach (`Update.h` vs. manual
  `esp_ota_*`) for `Esp32OtaPlatform`.
- HTTPS client choice for OTA downloads.
- **Whether/how `claim_code` and product ownership claim
  (`docs/communication-protocol.md` sections 4, 4.1, 6.1, 9.1) should be
  represented in `DeviceIdentity` and the BLE provisioning payload -
  still open; the 2026-08-11 reconciliation pass specified the QR's wire
  format but deliberately did not implement it in firmware.**
- Environment separation (DEV/PROD) - `AwsIotConnectionConfig`
  (`network/mqtt_transport.h`) has no environment field yet.
- Watchdog/recovery strategy for the core `StateMachine`'s `Error` state
  (still a dead end, unchanged from the prior pass).

*(Resolved 2026-08-11: `ProtocolErrorCode` vs. doc reconciliation, and
`issued_at`/`expires_at` format - see Decisions.)*

## Technical Debt

*(Prior-pass items - `main.cpp` never actually connects Wi-Fi with real
credentials (now partially addressed: `ProvisioningManager` does call
`wifi.begin()` once credentials arrive via BLE/button flow, but nothing
provisions real BLE credentials yet since `Esp32BleProvisioning` is a
stub); `Intercom::requestDoorOpen()` has no pulse timing;
`updateStateMachine()` is a no-op placeholder - all still true. New from
this pass:)*

- `CommandHandler`'s synchronous single-response model (see Decisions)
  should become a real two-phase `ACCEPTED` → terminal-response lifecycle
  once door actuation can be genuinely asynchronous.
- `FleetProvisioningCoordinator` is constructed in `main.cpp` but never
  invoked - the "Wi-Fi connected, no certificate stored yet → run Fleet
  Provisioning before attempting MQTT" orchestration doesn't exist. This
  is arguably the single biggest gap between "the pieces exist" and "the
  device can actually provision itself."
  its ConnectingWifi phase - if Wi-Fi never connects after credentials
  are received, it stays there indefinitely. No `ProvisioningFailed`
  path exists yet despite the event being defined.
- `DOOR_OPENED`/`DOOR_OPEN_FAILED` protocol events are defined but never
  published - `main.cpp` doesn't currently translate a command handler's
  door-actuation result into one of these events (only `OTA_COMPLETED`/
  `OTA_FAILED`/`FACTORY_RESET_REQUESTED`/`PROVISIONING_*` are wired to
  event publishing today).
- `JobsCoordinator`/OTA flow doesn't publish `OTA_STARTED` before running
  a job - only the terminal `OTA_COMPLETED`/`OTA_FAILED`.
- See the Decisions > "Protocol doc v1.1 reconciliation" entry - the
  `ProtocolErrorCode` set, `issued_at`/`expires_at` format, and
  `claim_code` concept all need a deliberate reconciliation pass against
  the now-authoritative doc.
- `docs/architecture.md` and this file must be kept manually in sync with
  the code; no automated drift check exists.

## Future Work

*(Prior-pass items - characterize the intercom circuit, implement ring
detection, choose a communication protocol/audio codec - are now
partially superseded/refined by this pass's decisions; the concrete next
steps are the Open Questions and Technical Debt items above, plus:)*

- Reconcile the implementation against `docs/communication-protocol.md`
  v1.1 (error codes, `issued_at`/`expires_at` format, `claim_code`).
- Wire `FleetProvisioningCoordinator` into `main.cpp`'s connection flow.
- Choose and implement a real MQTT 3.1.1/TLS client for
  `Esp32AwsIotTransport`.
- Implement NTP/time sync so `Esp32Clock::hasValidTime()` can become
  real, unblocking remote command handling end-to-end.
- Implement `NvsStore` for real so the dedup cache, event outbox, device
  identity, and credentials genuinely survive reboot.
- Choose a firmware signing scheme; implement real SHA-256 (mbedtls,
  available on-device) and signature verification in
  `DefaultFirmwareVerifier`.
- Implement `Esp32OtaPlatform` (HTTPS download + ESP32 OTA partitions).
- Decide the BLE provisioning framework question (ESP-IDF vs. hand-rolled
  Arduino BLE) and implement `Esp32BleProvisioning` for real.
- Wire `wifi_rssi`/`free_heap` into `HealthReport`/Device Shadow for
  real.
- Add a `ProvisioningFailed` path/timeout to `ProvisioningManager`.
- Publish `DOOR_OPENED`/`DOOR_OPEN_FAILED`/`OTA_STARTED` events from
  `main.cpp`.
- Once real PlatformIO/toolchain access is available, run
  `pio run -e esp32-c3` and `pio test -e native` as the authoritative
  build/test verification (see Tests for what substituted for this).

## Tests

24 native test suites now exist under `test/` (up from 5), covering every
module that doesn't require real hardware, AWS, or crypto:

| Test dir | Covers |
|---|---|
| `test_state_machine`, `test_events`, `test_line_detector`, `test_protocol`, `test_audio` | Unchanged from the prior pass (`test_line_detector` updated for the new `setDoorOutput()` bool signature, plus 2 new `Intercom::requestDoorOpen()` tests) |
| `test_mqtt_topics` | Every `MqttTopics` method, incl. the empty-string-when-unconfigured Fleet Provisioning case |
| `test_command_parser` | Valid command with Unix timestamps, ISO-8601 string timestamp correctly NOT accepted, malformed JSON, missing/invalid-format/uppercase command_id, missing/unsupported protocol_version, unknown command, oversized payload, payload object capture, reserved commands, every `ProtocolErrorCode`'s string + default message, all 5 canonical `ProtocolIntercomState` values (added/expanded 2026-08-11 reconciliation pass) |
| `test_command_handler` | OPEN_DOOR success/failure, clock-not-trustworthy/expired/oversized-window rejection, RESTART, duplicate OPEN_DOOR not re-actuating hardware, ENTER_PROVISIONING/FACTORY_RESET/reserved/unknown rejection |
| `test_command_cache` | In-memory find/record/eviction, persistent round-trip across a simulated reboot |
| `test_event_outbox` | Enqueue/pending/dequeue, capacity eviction, duplicate-ID upsert, persistent round-trip preserving `event_id` |
| `test_reconnect_manager` | Backoff bounds/growth/clamp-to-max, reset, attempt counter |
| `test_button` | Debounce/bounce rejection, short press, 3s/10s one-shot thresholds, no repeat while held, re-fire after release |
| `test_device_identity` | Format validation, first-boot generation, stability across reload, provisioned-flag persistence |
| `test_persistent_store` | `MemoryStore` get/set/remove/overwrite, `DeviceCredentialStore` isolation + safe logging |
| `test_clock` | `FakeClock` monotonic/wall-time behavior |
| `test_random_id` | ID format, determinism from a seed, uniqueness across calls |
| `test_device_shadow` | Reported field serialization, delta parsing, unknown-field tolerance, malformed-payload safety |
| `test_jobs` | No-job case, success path (status update sequence), failure path with reason |
| `test_ota` | Version comparison, success path, and every individual failure mode (version/download/hash/signature/install/boot), plus `DefaultFirmwareVerifier`'s fail-closed signature check |
| `test_health_reporter` | First-call-due, cadence, `forceNextPublish()` |
| `test_provisioning` | Boot-time entry (missing vs. present credentials), credential receipt → Wi-Fi connect → completion, re-trigger no-op while in progress |
| `test_fleet_provisioning` | Full success path, and each individual failure mode (keygen/cert-request/register-thing) |
| `test_factory_reset` | Wi-Fi/provisioned-flag cleared, identity/credentials preserved |
| `test_mqtt_transport` | `FakeDeviceTransport` connect/publish/subscribe/deliver, armed connect failures |

**How this was validated (no PlatformIO/gcc in this environment - same
constraint as the prior pass):**
- All 24 test suites, plus the ~33 native-safe `.cpp` files under `src/`
  (everything except `main.cpp` and `network/wifi.cpp`), were compiled
  **and executed** with MSVC (`cl.exe`, VS 2022 Build Tools) against the
  real ArduinoJson v7 source (fetched into a local scratch directory, not
  committed to the repo) and the same throwaway Unity-compatible shim
  used in the prior pass. **137 tests, 0 failed**, re-confirmed after the
  2026-08-11 protocol reconciliation pass (up from 130 after
  `test_command_parser` gained 7 tests for command_id format, ISO-8601-
  string timestamp rejection, error code strings/messages, and canonical
  intercom states).
- `main.cpp` and every Arduino-dependent `.cpp` file (`wifi.cpp`,
  `clock.cpp`, `system_control.cpp`, `random_id.cpp`, `gpio.cpp`,
  `button.cpp`, `ble_provisioning.cpp`) were additionally compiled under
  a `-DARDUINO=100` flag against extended `Arduino.h`/`WiFi.h`/
  `esp_system.h` shims (also scratch-only) - all compiled cleanly.
- Four files that combine `ARDUINO` **and** ArduinoJson
  (`device_shadow.cpp`, `messages.cpp`, `command_cache.cpp`,
  `event_outbox.cpp`) could not be validated under the `-DARDUINO=100`
  flag: ArduinoJson's PROGMEM polyfill expects real AVR/ESP `pgm_read_byte`-
  style macros that only exist in the actual arduino-esp32 framework, not
  in a hand-written shim, and reproducing them was judged not worth the
  effort. **This is a sandbox/shim limitation, not a known code defect** -
  the exact same ArduinoJson API calls in those files were fully compiled
  and executed (with passing tests) in the native, non-`ARDUINO` build.
- **Not verified, same as the prior pass:** an actual `pio run -e esp32-c3`
  against the real `espressif32` platform/toolchain, or `pio test -e native`
  via PlatformIO+Unity+gcc specifically. Do this before relying on the
  `esp32-c3` environment or the new `lib_deps` entry
  (`bblanchon/ArduinoJson@^7.0.0`) actually resolving correctly.
- **Hardware/AWS/crypto-dependent, not testable at all yet:** anything
  exercising `Esp32GpioHardware`, `Esp32AwsIotTransport`,
  `Esp32BleProvisioning`, `Esp32OtaPlatform`, `Esp32KeyPairGenerator`,
  `Esp32FleetProvisioningTransport`, `NvsStore`, or real Wi-Fi/NTP.

## Hardware Dependencies

*(Prior-pass items unchanged: `Esp32GpioHardware`, real Wi-Fi connect,
future `IAudioIO`, the `esp32-c3` PlatformIO environment itself. New from
this pass:)*

- `Esp32AwsIotTransport` — needs an MQTT 3.1.1/TLS client library choice
  and real AWS IoT infrastructure to validate against.
- `NvsStore` — needs real Preferences/NVS wiring and on-hardware
  validation of size limits (certificates/keys/outbox entries).
- `Esp32RandomSource` — needs on-hardware validation that `esp_random()`
  behaves as expected in this build configuration.
- `Esp32Clock` — needs NTP integration and on-hardware time-sync
  validation.
- `Esp32ButtonInput`, `Esp32SystemControl` (`ESP.restart()`),
  `Esp32BleProvisioning`, `Esp32OtaPlatform`, `Esp32KeyPairGenerator`,
  `Esp32FleetProvisioningTransport` — all stubs needing real hardware/
  library integration before any validation is possible.

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
  (verified via MSVC + a local Unity-compatible shim).
- `README.md` and `docs/architecture.md`.

**Second pass, same day — AWS IoT Core integration layer:**

Implemented:
- `core/random_id.h/.cpp` — secure 128-bit ID generation.
- `hardware/clock.h/.cpp`, `hardware/button.h/.cpp`,
  `hardware/system_control.h/.cpp`.
- `storage/persistent_store.h`, `memory_store.*`, `nvs_store.*` (stub),
  `credential_store.*`.
- `provisioning/device_identity.*`, `ble_provisioning.*` (stub),
  `provisioning_manager.*`, `fleet_provisioning.*`,
  `factory_reset_coordinator.*`.
- `network/mqtt_topics.*`, `reconnect_manager.*`, `mqtt_transport.*`
  (`Esp32AwsIotTransport` stub), `health_reporter.*`. `network/wifi.h`
  changed to expose `IWifiConnection`; added `network/wifi_fake.cpp`.
- `protocol/messages.*`, `command_cache.*`, `command_handler.*`,
  `event_outbox.*`.
- `aws/device_shadow.*`, `aws/jobs.*`.
- `ota/firmware_validation.*`, `ota/ota_manager.*`.
- **Breaking change**: `IHardwareIO::setDoorOutput()` and
  `Intercom::requestDoorOpen()` now return `bool` instead of `void`, so
  `OPEN_DOOR` can honestly report failure against stub hardware.
- `main.cpp` rewired to compose all of the above.
- 19 new native test suites (24 total), ~130 assertions, all passing
  (verified via MSVC + real ArduinoJson v7 source + Unity-compatible
  shim; ARDUINO-branch-only files also compiled under a `-DARDUINO=100`
  shim, with 4 ArduinoJson+ARDUINO combination files exempted for a
  documented shim limitation — see Tests).
- `platformio.ini`: added `lib_deps = bblanchon/ArduinoJson@^7.0.0` to
  both environments.

Still needed because of this change:
- Reconciliation against `docs/communication-protocol.md` v1.1, which was
  rewritten by the user mid-pass and is more detailed than what this code
  was built against — see Decisions.
- Everything under Open Questions and Technical Debt above.
- Real verification via `pio run -e esp32-c3` / `pio test -e native` in
  an environment with PlatformIO installed (not available in this
  session).

Next steps:
- Do the v1.1 reconciliation pass (error codes, timestamp format,
  claim_code).
- Wire `FleetProvisioningCoordinator` into `main.cpp`.
- Pick and implement the MQTT/TLS client, NTP sync, and NVS backend — in
  that rough priority order, since NTP gates all remote command handling
  and NVS gates every persistence guarantee this pass built the
  architecture for.

**Third pass, same day — protocol reconciliation (no AWS/MQTT/NTP/BLE/NVS
implementation; doc+firmware consistency only):**

Implemented:
- `docs/communication-protocol.md` bumped to v1.2 with a revision note
  explaining scope.
- Command `issued_at`/`expires_at` confirmed/documented as Unix epoch
  seconds everywhere (doc examples fixed; code already matched - no code
  change needed here). New section 14.1 explicitly distinguishes command
  timestamps (Unix seconds) from event timestamps (ISO-8601).
- `ProtocolErrorCode` (`protocol/messages.h/.cpp`) gained `NotProvisioned`,
  `WifiUnavailable`, `CloudUnavailable` to match the doc's canonical list
  exactly; every code now has an origin (device/backend/application)
  documented in both the header and a new table in doc section 21.
- `command_id` format validation: `isValidCommandId()` added
  (`protocol/messages.h/.cpp`), enforcing exactly 32 lowercase hex
  characters, no prefix; `parseCommand()` now rejects malformed IDs.
- `ProtocolIntercomState` enum + `toString()` added
  (`protocol/messages.h/.cpp`): `IDLE`/`RINGING`/`OFF_HOOK`/`IN_CALL`/
  `ERROR`, a closed vocabulary separate from `core::State`.
  `main.cpp` gained `toProtocolIntercomState()` and now uses it for
  `HealthReport.intercomState`/`ShadowReportedState.intercomState`
  instead of `core::State`'s diagnostic `toString()` (which could
  previously have leaked `"BOOT"` onto the wire).
- `docs/communication-protocol.md` section 4.1 (new): canonical QR code
  URI format (`interbridge://claim?v=1&device_id=ib-<32hex>&claim_code=<secret>`)
  with explicit validation rules. **Documentation only** - no QR
  scanning/parsing code added to firmware, per the task's instruction.
- All JSON example command_id/device_id values across the doc corrected
  to valid formats (`0123456789abcdef0123456789abcdef` /
  `ib-0123456789abcdef0123456789abcdef`); all 9 JSON code blocks in the
  doc verified to be syntactically valid JSON (`python -m json.loads`
  over every fenced block).
- `test_command_parser` grew from 9 to 16 tests: Unix-timestamp success,
  ISO-8601-string timestamp correctly rejected (treated as absent),
  invalid/uppercase `command_id` rejection, `isValidCommandId()` unit
  tests, every `ProtocolErrorCode` string + non-empty default message,
  all 5 canonical `ProtocolIntercomState` strings.
- Full suite re-run: **137 tests, 0 failed** (up from 130), same MSVC +
  real-ArduinoJson-source method as the prior two passes (`pio test`/
  `pio run` still unavailable in this environment - see Tests).

Explicitly NOT done in this pass (by instruction): no AWS IoT Core, MQTT/
TLS, NTP, BLE, real NVS, Lambda, API Gateway, or Cognito implementation.
Those remain exactly as stubbed as before - see Known Limitations and
Hardware Dependencies, both still accurate.

Still needed because of this change:
- `claim_code`/product-ownership-claim representation in `DeviceIdentity`
  and the BLE provisioning payload (doc now specifies the wire format;
  firmware still doesn't carry it).
- Environment (DEV/PROD) separation in `AwsIotConnectionConfig`.
- Everything else already listed under Open Questions/Technical Debt/
  Future Work (unchanged by this pass - it was protocol-consistency-only).

Next steps: Phase 1 AWS infrastructure work (MQTT/TLS client, NTP, real
NVS, BLE) can now proceed against a protocol document and firmware that
agree with each other on the wire-level details reconciled here.
