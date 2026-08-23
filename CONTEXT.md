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
Device Shadow), OTA updates (AWS IoT Jobs), and **BLE-first onboarding**:
nearby BLE discovery is the primary way a user sets up a device; QR
scanning and typing a 12-digit `setup_code` are fallback ways to resolve
the same physical device, not separate provisioning flows - see
`docs/communication-protocol.md` section 7.

## Current Status

**Architectural foundation + an AWS IoT Core integration layer + a
BLE-first onboarding architecture.** Four implementation passes have
happened:

1. The initial skeleton (state machine, events, logger, hardware/
   intercom/audio abstractions) — hardware-independent, unit tested.
2. AWS IoT Core integration layer: device identity, persistent storage,
   MQTT topic building, protocol message models, command handling
   (time-safety + duplicate protection), event outbox, health telemetry,
   Device Shadow, AWS IoT Jobs/OTA, an initial BLE provisioning stub, AWS
   IoT Fleet Provisioning (CSR flow, not yet invoked), the physical
   config/reset button, and factory reset.
3. Protocol reconciliation pass: aligned `docs/communication-protocol.md`
   and the firmware on command timestamp format, the full error code set
   with per-code origin, the closed `intercom_state` vocabulary, and a
   QR claim-code URI format (superseded by pass 4's `setup_code` rename).
4. **This pass — BLE-first onboarding rework**: nearby BLE discovery
   promoted to the primary onboarding UX (QR/manual `setup_code` demoted
   to fallback identity-resolution only); `claim_code` renamed to
   `setup_code` throughout (12-digit human code, distinct from the
   temporary application claim session and the AWS Fleet Provisioning
   temporary claim); a dedicated 9-state `ProvisioningManager` state
   machine with a 5-minute provisioning window and in-window retry;
   `FleetProvisioningCoordinator` **now genuinely invoked** (resolves the
   prior pass's biggest Technical Debt item); a BLE advertisement model
   (`BleAdvertisementInfo`); a `BleSecurityMode` enum with no plaintext
   option; and a semantic LED status-indication interface.

All built against interfaces, with real logic where it doesn't require
unavailable hardware/AWS/crypto, and clearly-labeled stubs where it does.

**The isolated, manually provisioned DEV smoke entry point can complete an AWS
IoT MQTT/mTLS connection and has been validated on a bench device. The
production composition root still cannot complete a real AWS IoT connection,
BLE provisioning session, or signed OTA update.** See
Hardware Dependencies and Open Questions.

**Protocol doc status:** as of pass 4, `docs/communication-protocol.md`
(now v1.3) and the firmware agree on the onboarding architecture,
terminology (`setup_code`), and state vocabulary described in this file.
Backend/infrastructure-only topics (Cognito, API Gateway, environment
separation, the temporary application claim session's actual
implementation) remain out of scope for this repository - see Open
Questions.

## Architecture

See [`docs/architecture.md`](docs/architecture.md) for the full module
map. Summary of what exists now, by directory:

- `core/` — events, state machine, logger, version, `random_id.h` (secure
  128-bit ID generation). No Arduino dependency.
- `hardware/` — `gpio.h` (intercom line/door output), `clock.h`
  (monotonic + wall-clock time abstraction), `button.h` (config/reset
  button, debounced), `status_indicator.h` (semantic LED feedback, no
  GPIO chosen), `system_control.h` (restart).
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
- `provisioning/` — `device_identity.h` (`device_id` **and**
  `setup_code`, both load-or-generate-once), `ble_provisioning.h`
  (advertisement model **implemented**; real BLE stack **stub** -
  framework limitation, see Decisions), `provisioning_manager.h` (a
  dedicated 9-state onboarding coordinator, **implemented**),
  `fleet_provisioning.h` (CSR flow, **implemented** and now genuinely
  invoked by `provisioning_manager.h`), `factory_reset_coordinator.h`
  (preserves `device_id`/`setup_code`/credentials).
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
  (**implemented** with `256dpi/MQTT` + `WiFiClientSecure`, locally configured ATS endpoint and credentials).
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
- **Device identity + setup_code**: `provisioning/device_identity.h` -
  `DeviceIdentityProvider` now loads-or-generates **both** `device_id`
  (`ib-<32hex>`) and `setup_code` (12 decimal digits, via
  `core/random_id.h`'s new `generateNumericCode()`), neither ever
  regenerated once stored. `isValidSetupCode()` added alongside
  `isValidDeviceId()`. **Implemented** and unit tested (format
  validation, first-boot generation, stability across reload).
- **BLE provisioning**: `provisioning/ble_provisioning.h` -
  `BleAdvertisementInfo`/`buildBleAdvertisementInfo()` (**implemented**
  and unit tested: builds the non-secret `InterBridge-XXXX` advertisement
  from `device_id`'s last 4 hex chars) and `BleSecurityMode`
  (`Security1`/`Security2`, **no plaintext value exists in the enum by
  construction**). `Esp32BleProvisioning` (the real BLE
  advertise/session/credential-transfer implementation) is still a
  **stub**, and deliberately documented as such: the real design intent
  is ESP-IDF Unified Provisioning, which isn't exposed by the
  `framework = arduino` this project currently targets.
  `FakeBleProvisioning` (now with `isSessionActive()`/
  `setSessionActive()`) is fully implemented for tests.
- **Status indication**: `hardware/status_indicator.h` -
  `IStatusIndicator`/`ProvisioningIndication` (**implemented**: a closed
  set of semantic indications - `ProvisioningAvailable`, `AppConnected`,
  `ProvisioningSuccess`, `ProvisioningFailure`, `FactoryResetWarning` -
  with no GPIO/blink-pattern decision baked in). `Esp32StatusIndicator`
  is a **stub** (no LED GPIO chosen yet); `FakeStatusIndicator` is fully
  implemented for tests.
- **Provisioning orchestration**: `provisioning/provisioning_manager.h` -
  `ProvisioningManager` **reworked this pass** into a dedicated 9-state
  machine (`ProvisioningState`: `Idle`, `ProvisioningAvailable`,
  `BleSessionActive`, `ConnectingWifi`, `FleetProvisioning`,
  `CloudConnecting`, `Provisioned`, `ProvisioningFailed`,
  `NotProvisioned`) - **implemented** and unit tested (12 tests): boot-time
  entry, BLE session start/end, credential receipt → Wi-Fi connect →
  Fleet Provisioning (or skipped if a certificate already exists) →
  completion, in-window retry after a Fleet Provisioning failure, the
  5-minute provisioning window (`kProvisioningWindowMs`) expiring back to
  `Idle`/`NotProvisioned` depending on prior state, and the button
  re-opening provisioning on an already-configured device. Drives
  `IStatusIndicator` at each transition.
- **Fleet Provisioning (CSR flow)**: `provisioning/fleet_provisioning.h` -
  `FleetProvisioningCoordinator` (**implemented** and unit tested against
  fakes). `Esp32KeyPairGenerator`/`Esp32FleetProvisioningTransport` are
  **stubs** (no crypto library, no AWS account/template). **Now genuinely
  invoked** by `ProvisioningManager::advanceAfterWifiConnected()` - the
  prior pass's biggest Technical Debt item is resolved.
- **Factory reset**: `provisioning/factory_reset_coordinator.h` -
  `FactoryResetCoordinator` (**implemented** and unit tested: clears
  Wi-Fi credentials + provisioned flag, preserves `device_id`,
  `setup_code`, and AWS credentials).
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
infrastructure work outside firmware scope for now): environment
separation (DEV/PROD) in `AwsIotConnectionConfig`; Cognito/API Gateway/
Lambda application backend; ownership transfer/decommissioning backend
logic; AWS account/region/rule names/Fleet Provisioning template. None of
these were in scope for this reconciliation pass (which was explicitly
protocol-consistency-only, not infrastructure), and none were touched.

*(Update, BLE-first onboarding pass: `claim_code` → `DeviceIdentity` was
resolved in the very next pass - see the decisions immediately below.
The QR URI's `claim_code` parameter was also renamed to `setup_code` -
`docs/communication-protocol.md` section 4.3 reflects the current,
correct name; treat any remaining mention of `claim_code` above as
historical record of what pass 3 actually did, not current guidance.)*

### Decision: onboarding is BLE-first; `claim_code` renamed to `setup_code`
Motivo: explicit architecture change requested - nearby BLE discovery
becomes the primary onboarding UX (matching how the app can already
address the physical device once it's advertising), with QR/manual entry
demoted to fallback identity-resolution methods that converge on the
same BLE session. The rename addresses a real terminology problem the
prior pass's `claim_code` had: `docs/communication-protocol.md` v1.1
described it as requiring "128 bits of cryptographically secure
randomness" and being "single-use," but a 128-bit value isn't something
a human types or a small QR-adjacent code conveys well, and "single-use"
doesn't fit a value printed once on a physical unit.
Consequência: `setup_code` is now explicitly a **low-stakes, permanent,
12-decimal-digit** identifier (`DeviceIdentity.setupCode`,
`core/random_id.h`'s `generateNumericCode()`) - its job is physical-unit
disambiguation, not authentication. The actual security boundary for
onboarding moved to where it always should have been: the BLE session
(Protocomm security mode + PoP) and a backend-authenticated temporary
claim session, both already conceptually present before but now named
and separated explicitly (see `docs/communication-protocol.md` section
4's terminology note). This resolves the "Still genuinely open" item
directly above from the prior pass.

### Decision: `ProvisioningManager` reworked into a dedicated 9-state machine, now invoking Fleet Provisioning
Motivo: the prior 5-state `ProvisioningManager` (`Idle`/
`AwaitingCredentials`/`ConnectingWifi`/`Completed`/`Failed`) didn't model
BLE session lifecycle separately from advertising, didn't have a
recoverable failure/retry path, had no timeout, and - most importantly -
never actually called `FleetProvisioningCoordinator` (constructed in
`main.cpp` but unused, flagged as the top Technical Debt item in the
prior pass).
Consequência: the new state set
(`Idle`/`ProvisioningAvailable`/`BleSessionActive`/`ConnectingWifi`/
`FleetProvisioning`/`CloudConnecting`/`Provisioned`/`ProvisioningFailed`/
`NotProvisioned`) makes each of these a first-class, testable transition.
`ProvisioningManager` now takes `DeviceCredentialStore&` (to check
whether a certificate already exists, skipping Fleet Provisioning on
Wi-Fi-only re-provisioning) and `FleetProvisioningCoordinator&` as
constructor dependencies. `update()`/`checkAtBoot()`/`requestProvisioning()`
now take an explicit `nowMs` parameter (not an `IClock&` member) purely
because every timing decision in this class only needs "the current
time," not the full clock interface - the same pattern as
`HealthReporter`/`ButtonController`/`ReconnectManager`.

### Decision: `CloudConnecting` is a hand-off state, not a verified connection
Motivo: gating `ProvisioningManager`'s success on an actual
`IDeviceTransport.connect()` call would mean provisioning can never
succeed on real hardware today, since `Esp32AwsIotTransport::connect()`
always returns `false` (no MQTT/TLS client exists yet) - and it would
duplicate `main.cpp`'s existing `ReconnectManager`-based connect/backoff
loop.
Consequência: `ProvisioningManager` considers itself done
(`Provisioned`) once Wi-Fi is connected and an AWS IoT certificate is in
place (either pre-existing or freshly obtained via Fleet Provisioning).
The actual first MQTT connection remains `main.cpp`'s
`updateNetwork()`'s job, run independently on every loop iteration
regardless of provisioning state. This is documented as a deliberate
scope boundary, not an oversight - see
`docs/communication-protocol.md` section 7.5.

### Decision: `BleSecurityMode` has no plaintext/"None" value
Motivo: explicit instruction to never silently downgrade to insecure
provisioning, even by omission (e.g. an unhandled `default:` case
defaulting to something insecure).
Consequência: the enum only has `Security1`/`Security2` - there is
structurally no value a future implementation could select (accidentally
or otherwise) that means "no security." `Esp32BleProvisioning` defaults
to requesting `Security2` (constructor parameter), documented as
needing on-hardware/mobile validation for whether the pinned ESP-IDF
version supports it cleanly (`Security1` is the documented fallback).

### Decision: LED feedback is a semantic interface (`IStatusIndicator`), not a GPIO abstraction
Motivo: the LED GPIO and blink/color design are explicitly not defined
yet (hardware not finalized), but callers (`ProvisioningManager`,
`main.cpp`'s button handler) already know *what they want to communicate*
(`ProvisioningAvailable`, `AppConnected`, `ProvisioningSuccess`,
`ProvisioningFailure`, `FactoryResetWarning`) independent of how it's
ultimately rendered.
Consequência: `hardware/status_indicator.h`'s `IStatusIndicator` takes a
`ProvisioningIndication` enum value, never a GPIO/color/timing
parameter. `Esp32StatusIndicator` is a stub (does nothing) until the LED
hardware is chosen; `FakeStatusIndicator` records the latest indication
for tests. This mirrors the existing `IHardwareIO`/`IClock` pattern of
modeling intent, not mechanism.

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
  by `framework = arduino`. See Decisions and
  `docs/communication-protocol.md` section 7.2. This means the entire
  BLE-first onboarding flow, however well-tested against fakes, cannot
  run end-to-end on real hardware yet - nearby discovery, the security
  session, and credential transfer are all unimplemented at the
  ESP32/BLE-stack level.
- `DefaultFirmwareVerifier::verifySignature()` always returns `false` -
  no signing scheme/public key chosen. Real OTA cannot complete
  end-to-end even once download/transport exist, until this changes.
- `Esp32KeyPairGenerator`/`Esp32FleetProvisioningTransport` are stubs -
  no on-device crypto library for keypair/CSR generation, no AWS account/
  template. `FleetProvisioningCoordinator` itself is fully implemented,
  tested against fakes, and **is now genuinely invoked** by
  `ProvisioningManager` - but on real hardware it will always fail at the
  `Esp32KeyPairGenerator`/`Esp32FleetProvisioningTransport` stub, meaning
  `ProvisioningManager` will always end in `ProvisioningFailed` (which is
  the honest outcome given these stubs, not a bug).
- `network/health_reporter.h`'s cadence logic is solid, but the actual
  `HealthReport` built in `main.cpp` hardcodes `wifiRssi = 0` and
  `freeHeapBytes = 0` - not wired to real readings.
- This environment (the sandbox this pass was implemented in) has no
  PlatformIO, gcc, or clang, and no internet-connected `pio` CLI was
  used - see Tests for exactly what substituted for `pio run`/`pio test`.

## Open Questions

*(Board/GPIOs/intercom electrical interface/audio hardware/audio codec/
OTA artifact hosting remain exactly as open as in the initial pass - see
`docs/communication-protocol.md` section 37 for the user's own, more
complete "Still Open" list, which is the more authoritative version of
this section going forward.)*

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
- **Whether Protocomm Security 2 is cleanly supported by the pinned
  ESP-IDF version** (`Security1` is the documented fallback -
  `BleSecurityMode`, `docs/communication-protocol.md` section 7.2), and
  whether to switch `platformio.ini`'s `esp32-c3` environment to
  `framework = espidf` (or a mixed framework) to get ESP-IDF Unified
  Provisioning for real at all.
- Real BLE service/characteristic UUIDs.
- Proof-of-Possession generation/persistence strategy (currently
  regenerated every boot in `main.cpp`, not stored).
- Temporary application claim session design (issuance, expiry,
  validation) - a backend/app concern; firmware only ever receives
  whatever Fleet Provisioning claim material arrives over the already-
  established BLE session, and doesn't model the claim session itself.
- ESP32 OTA partition/library approach (`Update.h` vs. manual
  `esp_ota_*`) for `Esp32OtaPlatform`.
- HTTPS client choice for OTA downloads.
- Environment separation (DEV/PROD) - `AwsIotConnectionConfig`
  (`network/mqtt_transport.h`) has no environment field yet.
- Watchdog/recovery strategy for the core `StateMachine`'s `Error` state
  (still a dead end, unchanged from the initial pass).

*(Resolved across passes 3-4: `ProtocolErrorCode` vs. doc reconciliation;
`issued_at`/`expires_at` format; `claim_code` → `setup_code` rename and
`DeviceIdentity` representation; `FleetProvisioningCoordinator` wiring;
`ProvisioningManager` timeout/retry - see Decisions.)*

## Technical Debt

*(Prior-pass items still true: `Intercom::requestDoorOpen()` has no pulse
timing; `updateStateMachine()` is a no-op placeholder. New/updated from
this pass:)*

- `CommandHandler`'s synchronous single-response model (see Decisions)
  should become a real two-phase `ACCEPTED` → terminal-response lifecycle
  once door actuation can be genuinely asynchronous.
- `ConnectingWifi` still has no dedicated Wi-Fi-specific failure/timeout
  detection (`IWifiConnection` only reports connected/not-connected, not
  "failed to connect") - it's now caught eventually by the overall
  5-minute provisioning window (`kProvisioningWindowMs`) rather than
  hanging forever, which is a real improvement over the prior pass, but a
  shorter, dedicated Wi-Fi timeout would give faster user feedback.
- `DOOR_OPENED`/`DOOR_OPEN_FAILED` protocol events are defined but never
  published - `main.cpp` doesn't currently translate a command handler's
  door-actuation result into one of these events (only `OTA_COMPLETED`/
  `OTA_FAILED`/`FACTORY_RESET_REQUESTED`/`PROVISIONING_*` are wired to
  event publishing today).
- `JobsCoordinator`/OTA flow doesn't publish `OTA_STARTED` before running
  a job - only the terminal `OTA_COMPLETED`/`OTA_FAILED`.
- `docs/architecture.md` and this file must be kept manually in sync with
  the code; no automated drift check exists.
- `RemoteCommandProcessor`'s response outbox is RAM-only (`kMaxOutboxSize`,
  bounded) - a reboot while a response is still queued loses it, same
  tradeoff as the in-memory event outbox (section 17). Production should
  eventually back it with NVS the same way `PersistentDedupCache` does.
- The hypothesis that reusing the same `WiFiClientSecure`/socket across a
  broken MQTT session is what produced `setSocketOption(): ... Bad file
  number` after a publish failure is based on documented ESP32 Arduino
  core behavior and the vendored `256dpi/MQTT` source, not a real-hardware
  A/B test - see docs/mqtt-dev-smoke-test.md and the PR that introduced
  the fresh-`WiFiClientSecure`-per-reconnect change. Needs bench
  confirmation that the error no longer appears under the same
  few-consecutive-commands repro.

## Future Work

*(Prior-pass items - characterize the intercom circuit, implement ring
detection, choose an audio codec - remain exactly as open. The concrete
next steps are the Open Questions and Technical Debt items above, plus:)*

- Choose and implement a real MQTT 3.1.1/TLS client for
  `Esp32AwsIotTransport`.
- Implement NTP/time sync so `Esp32Clock::hasValidTime()` can become
  real, unblocking remote command handling end-to-end.
- Implement `NvsStore` for real so the dedup cache, event outbox, device
  identity/setup_code, and credentials genuinely survive reboot.
- Choose a firmware signing scheme; implement real SHA-256 (mbedtls,
  available on-device) and signature verification in
  `DefaultFirmwareVerifier`.
- Implement `Esp32OtaPlatform` (HTTPS download + ESP32 OTA partitions).
- Decide the BLE provisioning framework question (ESP-IDF vs. hand-rolled
  Arduino BLE) and implement `Esp32BleProvisioning` for real, including
  real advertising (`BleAdvertisementInfo`), a real Protocomm session,
  and real `isSessionActive()`/credential delivery.
- Choose/implement the on-device keypair+CSR crypto for
  `Esp32KeyPairGenerator`, and the AWS Fleet Provisioning MQTT wiring for
  `Esp32FleetProvisioningTransport`.
- Wire `wifi_rssi`/`free_heap` into `HealthReport`/Device Shadow for
  real.
- Implement `Esp32StatusIndicator` once LED hardware is chosen.
- Publish `DOOR_OPENED`/`DOOR_OPEN_FAILED`/`OTA_STARTED` events from
  `main.cpp`.
- Add a dedicated, shorter Wi-Fi-connect timeout inside `ConnectingWifi`
  (currently only caught by the overall provisioning window).
- Once real PlatformIO/toolchain access is available, run
  `pio run -e esp32-c3` and `pio test -e native` as the authoritative
  build/test verification (see Tests for what substituted for this).

## Tests

26 native test suites now exist under `test/` (up from 5 initially),
covering every module that doesn't require real hardware, AWS, or
crypto:

| Test dir | Covers |
|---|---|
| `test_state_machine`, `test_events`, `test_line_detector`, `test_protocol`, `test_audio` | Unchanged since the initial pass |
| `test_mqtt_topics` | Every `MqttTopics` method, incl. the empty-string-when-unconfigured Fleet Provisioning case |
| `test_command_parser` | Valid command with Unix timestamps, ISO-8601 string timestamp correctly NOT accepted, malformed JSON, missing/invalid-format/uppercase command_id, missing/unsupported protocol_version, unknown command, oversized payload, payload object capture, reserved commands, every `ProtocolErrorCode`'s string + default message, all 5 canonical `ProtocolIntercomState` values |
| `test_command_handler` | OPEN_DOOR success/failure, clock-not-trustworthy/expired/oversized-window rejection, RESTART, duplicate OPEN_DOOR not re-actuating hardware, ENTER_PROVISIONING/FACTORY_RESET/reserved/unknown rejection |
| `test_command_cache` | In-memory find/record/eviction, persistent round-trip across a simulated reboot |
| `test_event_outbox` | Enqueue/pending/dequeue, capacity eviction, duplicate-ID upsert, persistent round-trip preserving `event_id` |
| `test_reconnect_manager` | Backoff bounds/growth/clamp-to-max, reset, attempt counter |
| `test_button` | Debounce/bounce rejection, short press, 3s/10s one-shot thresholds, no repeat while held, re-fire after release |
| `test_device_identity` | `device_id`/`setup_code` format validation, first-boot generation of both, stability across reload, provisioned-flag persistence |
| `test_persistent_store` | `MemoryStore` get/set/remove/overwrite, `DeviceCredentialStore` isolation + safe logging |
| `test_clock` | `FakeClock` monotonic/wall-time behavior |
| `test_random_id` | Hex ID format/determinism/uniqueness, plus numeric-code (`setup_code`) length/digits-only/determinism/display-grouping |
| `test_device_shadow` | Reported field serialization, delta parsing, unknown-field tolerance, malformed-payload safety |
| `test_jobs` | No-job case, success path (status update sequence), failure path with reason |
| `test_ota` | Version comparison, success path, and every individual failure mode (version/download/hash/signature/install/boot), plus `DefaultFirmwareVerifier`'s fail-closed signature check |
| `test_health_reporter` | First-call-due, cadence, `forceNextPublish()` |
| `test_provisioning` **(reworked this pass)** | Boot-time entry, BLE session active/dropped transitions, credential receipt → Wi-Fi connect → Fleet Provisioning (success/skip-if-cert-exists/failure-with-retry) → completion, 5-minute provisioning window expiry (both `NotProvisioned` and `Idle` outcomes), re-trigger no-op while in progress, button re-opening an already-provisioned device |
| `test_fleet_provisioning` | Full success path, and each individual failure mode (keygen/cert-request/register-thing) |
| `test_factory_reset` | Wi-Fi/provisioned-flag cleared, identity/credentials/**setup_code** preserved |
| `test_mqtt_transport` | `FakeDeviceTransport` connect/publish/subscribe/deliver, armed connect failures |
| `test_ble_provisioning` **(new)** | `buildBleAdvertisementInfo()` (hint extraction, no secrets), `BleSecurityMode` has no plaintext value, security mode configurability, advertise/session-active fakes |
| `test_status_indicator` **(new)** | `FakeStatusIndicator` show/clear/count behavior |

**How this was validated (no PlatformIO/gcc in this environment - same
constraint as every prior pass):**
- All 26 test suites, plus the 34 native-safe `.cpp` files under `src/`
  (everything except `main.cpp` and `network/wifi.cpp`), were compiled
  **and executed** with MSVC (`cl.exe`, VS 2022 Build Tools) against the
  real ArduinoJson v7 source (fetched into a local scratch directory, not
  committed to the repo) and the same throwaway Unity-compatible shim
  used throughout. **160 tests, 0 failed**, re-confirmed after this
  pass's BLE-first onboarding rework (up from 137 after the protocol
  reconciliation pass - the increase comes from `test_provisioning`
  growing from 5 to 12 tests and the two new suites above).
- `main.cpp` and every Arduino-dependent `.cpp` file were additionally
  compiled under a `-DARDUINO=100` flag against extended
  `Arduino.h`/`WiFi.h`/`esp_system.h` shims (also scratch-only) - all
  compiled cleanly. (`ble_provisioning.cpp`/`status_indicator.cpp` have
  no Arduino-specific code at all currently - pure stubs - so this check
  is trivially satisfied for them, same as the native run.)
- Files that combine `ARDUINO` **and** ArduinoJson (`device_shadow.cpp`,
  `messages.cpp`, `command_cache.cpp`, `event_outbox.cpp`) still can't be
  validated under `-DARDUINO=100`: ArduinoJson's PROGMEM polyfill expects
  real AVR/ESP `pgm_read_byte`-style macros only present in the actual
  arduino-esp32 framework, not a hand-written shim. **This is a
  sandbox/shim limitation, not a known code defect** - the exact same
  API calls in those files were fully compiled and executed (with
  passing tests) in the native, non-`ARDUINO` build.
- **Not verified, same as every prior pass:** an actual
  `pio run -e esp32-c3` against the real `espressif32` platform/
  toolchain, or `pio test -e native` via PlatformIO+Unity+gcc
  specifically - `pio`/`gcc` were re-confirmed absent from this
  environment at the start of this pass. Do this before relying on the
  `esp32-c3` environment.
- **Hardware/AWS/crypto-dependent, not testable at all yet:** anything
  exercising `Esp32GpioHardware`, `Esp32AwsIotTransport`,
  `Esp32BleProvisioning` (nearby discovery, real Protocomm session, real
  credential transfer), `Esp32StatusIndicator`, `Esp32OtaPlatform`,
  `Esp32KeyPairGenerator`, `Esp32FleetProvisioningTransport`,
  `NvsStore`, or real Wi-Fi/NTP.

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
  `Esp32BleProvisioning` (now covering nearby discovery, advertisement,
  a real Protocomm session, and credential transfer - the scope grew
  this pass even though the stub itself didn't change behavior),
  `Esp32StatusIndicator`, `Esp32OtaPlatform`, `Esp32KeyPairGenerator`,
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
  (`Esp32AwsIotTransport` was a stub in that pass), `health_reporter.*`. `network/wifi.h`
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

**Fourth pass, same day — BLE-first onboarding update:**

Implemented:
- `core/random_id.h/.cpp`: `generateNumericCode()` + `formatNumericCodeForDisplay()`
  (12-digit `setup_code` generation/display).
- `provisioning/device_identity.h/.cpp`: `DeviceIdentity.setupCode` +
  `isValidSetupCode()`; `DeviceIdentityProvider::load()` now loads-or-
  generates `setup_code` alongside `device_id`, resolving the prior
  pass's open "how should claim_code be represented in DeviceIdentity"
  question.
- `provisioning/ble_provisioning.h/.cpp`: `BleSecurityMode`
  (`Security1`/`Security2`, no plaintext value), `BleAdvertisementInfo` +
  `buildBleAdvertisementInfo()`, `IBleProvisioning::isSessionActive()`/
  `securityMode()`, `startAdvertising()` signature now takes the
  advertisement info. `FakeBleProvisioning` gained
  `setSessionActive()`/`lastAdvertisementInfo()`.
- `hardware/status_indicator.h/.cpp` (new): `IStatusIndicator`/
  `ProvisioningIndication`/`Esp32StatusIndicator` (stub)/
  `FakeStatusIndicator`.
- `provisioning/provisioning_manager.h/.cpp`: reworked from a 5-state to
  a 9-state machine (`Idle`/`ProvisioningAvailable`/`BleSessionActive`/
  `ConnectingWifi`/`FleetProvisioning`/`CloudConnecting`/`Provisioned`/
  `ProvisioningFailed`/`NotProvisioned`); added
  `kProvisioningWindowMs` (5 minutes); **now genuinely invokes
  `FleetProvisioningCoordinator`**; added in-window failure retry;
  `update()`/`checkAtBoot()`/`requestProvisioning()` now take `nowMs`.
  New constructor dependencies: `DeviceCredentialStore&`,
  `FleetProvisioningCoordinator&`, `IStatusIndicator&`,
  `BleAdvertisementInfo`.
- `main.cpp`: added `Esp32StatusIndicator`; reordered
  `fleetProvisioningCoordinator` construction before `provisioningManager`
  (the latter now holds a reference to the former); wired
  `buildBleAdvertisementInfo()`; all provisioning entry points now pass
  `clock.monotonicMs()`; button handler shows `FactoryResetWarning`
  before executing a factory reset.
- Doc renames/additions across `docs/communication-protocol.md` (now
  v1.3): `claim_code` → `setup_code` everywhere it meant the human-facing
  product code (14 of 16 occurrences changed; the terminology-note
  mention of the old name is intentional); QR URI parameter renamed to
  `setup_code`; new sections 7.1-7.7 (nearby discovery primary flow,
  security mode, QR/manual fallback, advertisement model, provisioning
  state machine, window/recovery, LED indication); section 6.1 rewritten
  so BLE discovery (not QR scanning) is the described trigger; section
  9.1's ownership-transfer text corrected (setup_code is permanent, so
  transfer reissues a claim *session*, not a new code); "Still Open"
  gained a BLE/mobile-validation subsection.
- `test_provisioning` fully rewritten (5 → 12 tests): BLE session
  active/inactive transitions, credential receipt, full flow through
  Fleet Provisioning, existing-certificate skip path, Fleet Provisioning
  failure + in-window retry, provisioning window expiry (both outcomes),
  request-while-in-progress no-op, button re-opening a configured
  device.
- `test_device_identity`, `test_random_id`, `test_factory_reset` extended
  for `setup_code` (format validation, generation, display grouping,
  survival through factory reset).
- Two new suites: `test_ble_provisioning` (6 tests), `test_status_indicator`
  (4 tests).
- Full suite re-run: **160 tests, 0 failed** (up from 137), same MSVC +
  real-ArduinoJson-source method as every prior pass (`pio`/`gcc`
  re-confirmed unavailable in this environment).

Explicitly NOT done in this pass (by instruction): no AWS IoT Core, real
MQTT/TLS, NTP, BLE stack, real NVS, Lambda, API Gateway, or Cognito
implementation.

Still needed because of this change:
- Everything under Open Questions/Technical Debt/Future Work above -
  most notably the BLE stack itself (`Esp32BleProvisioning`), which now
  has a considerably more detailed contract to implement against
  (advertisement, session tracking, security mode) but is still 100%
  stub.
- A dedicated, shorter Wi-Fi-connect timeout (currently only the overall
  5-minute window catches a stuck `ConnectingWifi`).

Next steps: with the onboarding architecture and terminology now stable
and doc/firmware-consistent, the highest-leverage next steps are (in
order) NTP (unblocks all remote commands), a real MQTT/TLS client
(unblocks everything network-facing), and the ESP-IDF Unified
Provisioning integration (unblocks the entire onboarding flow this pass
just designed).

## Phase 1D.1 — controlled DEV MQTT smoke path (2026-08-14)

A separate `esp32-c3-dev-mqtt` environment and guarded
`src/dev/mqtt_smoke_main.cpp` now implement a manual DEV-only Wi-Fi + AWS IoT
MQTT/mTLS smoke path using `256dpi/MQTT` and `WiFiClientSecure`. DEV rule names
are explicit (`interbridge_dev_ingest_rule` and
`interbridge_dev_response_rule`); shared topic configuration has no provisional
rule defaults. The pure `DevMqttSmokeHandler` parses protocol v1 and always
fails closed without any physical/system/provisioning dependencies.

This does not change the production composition root: `Esp32AwsIotTransport`,
BLE, NVS, Fleet Provisioning platform pieces, and production time sync remain
stubs. The smoke path's ignored local certificate/private key injection is only
for controlled bench validation and does not supersede production on-device key
creation + CSR. Backend Phase 1E Basic Ingest persistence, real AWS validation,
and ESP32 flashing remain pending.

Firmware CI now runs on pull requests and pushes to `main`, using Python 3.12
and PlatformIO 6.1.18. It performs a tracked-file credential scan, native tests,
the ordinary ESP32-C3 build, and a compile-only DEV smoke build. CI copies the
placeholder-only example to the ignored local secret-header path; it never
executes firmware, connects to Wi-Fi/AWS, uses GitHub secrets, or flashes
hardware. The corrected smoke contract is command subscribe QoS 1, health
publish QoS 0, event publish QoS 1, response publish QoS 1, and `retain=false`
for every publication. Hardware and AWS runtime validation remain pending.

## Phase 1D.2 — real ESP32-C3 DEV validation and recovery hardening (2026-08-16)

Phase 1D is complete in the narrow scope of the first controlled DEV MQTT/mTLS
device. A generic 4 MB ESP32-C3 Super Mini, temporarily built as
`esp32-c3-devkitm-1`, validated build, USB upload, native USB CDC, 2.4 GHz
Wi-Fi, MQTT/mTLS port 8883 with an individual X.509 certificate, ClientId from
`device_id`, QoS 1 command subscription, QoS 0 initial health, safe receipt and
rejection of `OPEN_DOOR` without physical action, response publication, and a
power-off/cold-boot reconnection followed by another command/response. Repeated
transient DHCP/DNS readiness failures recovered through retry; no endpoint IP or
public DNS resolver is hardcoded.

The smoke harness now uses a small hardware-independent five-state coordinator
(`WaitingForWifi`, `WaitingForDns`, `WaitingForTime`, `WaitingForMqtt`,
`Online`). It performs bounded, rollover-safe retry; gates DNS on DHCP network
configuration, gates TLS/MQTT on `NtpClock::hasValidTime()`, and repeats
subscription plus health publication after MQTT reconnect. Serial startup waits
only briefly for USB enumeration, remains headless, and emits a compact
credential-free heartbeat every 15 seconds. Both ESP32 environments define
`ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` for the validated native
USB path.

Still explicitly unvalidated: access-point loss and return while the board
remains powered, real BLE onboarding, NVS, Fleet Provisioning, Secure Boot,
Flash Encryption, intercom hardware, Phase 1E Basic Ingest persistence, and the
final custom PCB. The manually injected ignored DEV header remains bench-only
and does not supersede production key generation and provisioning.

## Phase 2D — fail-closed OPEN_DOOR logic (2026-08-21)

The backend portion of Phase 2D is implemented and awaits deployment and
validation. This firmware pass adds only the strict semantic parser, explicit
remote allowlist, persistent deduplication flow, and logical response publishing
through `IDeviceTransport`. At that pass, production `Esp32AwsIotTransport` remained stubbed;
the 2026-08-22 continuation below implements it, without real AWS validation.

The future command topic is `interbridge/{device_id}/commands` at QoS 1 without
wildcards. The strict payload requires protocol version 1, the exact local
`device_id`, a 32-character lowercase hexadecimal `command_id`, `OPEN_DOOR`, an
exactly empty `parameters` object, and integer epoch timestamps. Backend and
firmware validity are exactly 30 seconds, future clock tolerance is 5 seconds,
and an untrustworthy clock fails closed.

Door opening capability is modeled as `Disabled`, `Dtmf`, or `Relay`. `Disabled`
is the default and only operational value in this phase. `Dtmf` and `Relay` are
future placeholders only. A valid `OPEN_DOOR` produces `ACCEPTED` (accepted only
for processing), then terminal `REJECTED/CAPABILITY_DISABLED`. No COMPLETED,
DOOR_OPENED, DTMF, key, GPIO, relay, pulse, restart, reset, provisioning, or
physical action is produced. Configuration belongs to the Device and, when it
is implemented in a future phase, only an OWNER may change it.

Native logical integration uses `FakeDeviceTransport`, including exact-topic
subscription, callback delivery, QoS 1 responses, observable publish failures,
and required resubscription after reconnect. These tests do not prove real AWS
connectivity. The pre-change inventory was 28 test files and 170 tests; this
pass preserves every suite and adds a new suite rather than replacing one.


## Phase 2D transport continuation (2026-08-22)

The production `Esp32AwsIotTransport` is no longer a stub: it delegates MQTT/TLS to
`256dpi/MQTT` and `WiFiClientSecure`, validates ATS endpoint and `ib-<32hex>` identity,
loads certificate/key solely through `DeviceCredentialStore`, uses explicit short DEV
keepalive/timeout, and exposes an injected `IMqttClient` seam for offline tests. Existing
`RemoteCommandProcessor`, `CommandHandler`, persistent deduplication, `MqttTopics`, and
fail-closed policy were preserved. Exact-topic QoS 1 command subscription, pre-parser
8-KiB rejection, Basic Ingest QoS 1/non-retained response publication, Wi-Fi gating,
bounded jitter backoff, and reconnect subscription reset are explicit.

No AWS call, MQTT publication, certificate operation, provisioning, firmware flash, or
physical action was performed by Codex. The transport is compile/native-tested only.
Production NVS/SNTP/provisioning limitations still prevent claiming end-to-end completion.
Future hardware validation must observe `ACCEPTED` then
`REJECTED/CAPABILITY_DISABLED` through API → IoT → ESP32 → Basic Ingest → GET. The app
command UI remains disabled; DTMF, relay/GPIO/key sequences and opening configuration
remain deferred.

## Phase 2D DEV harness correction (2026-08-22)

PR #6 implemented and compiled `Esp32AwsIotTransport`, but its physical DEV entrypoint
still used direct MQTT/TLS objects and `DevMqttSmokeHandler`. The corrected harness now
loads the ignored local certificate/key into an explicitly transient `MemoryStore` and
composes `Esp32AwsIotTransport` -> `RemoteCommandProcessor` -> existing deduplication ->
`CommandHandler` with capability `Disabled`. Safe DEV hardware and system-control
implementations cannot actuate or restart. The old parallel handler was removed.

Codex performed no AWS call, MQTT publication, certificate operation, firmware flash, or
physical validation. Production NVS, Fleet Provisioning, BLE/onboarding, and the main
firmware's incomplete configuration remain unchanged. The required future physical
result is ordered `ACCEPTED` then `REJECTED/CAPABILITY_DISABLED` through Basic Ingest;
Phase 2D remains unvalidated until that complete real-device test passes.
