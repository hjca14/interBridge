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
  broken MQTT session produced `setSocketOption(): ... Bad file number` after
  a publish failure is now **bench-confirmed**: real hardware runs of the
  same few-consecutive-commands repro after the fresh-`WiFiClientSecure`-
  per-reconnect change no longer show that error. The socket-teardown fix
  itself is validated; it just wasn't the only issue at play (see below).
- Even with the socket fix, ACCEPTED and the terminal response were still
  published back-to-back within the same `processPending()` call whenever
  ACCEPTED succeeded, with no `transport.poll()` in between - two QoS 1
  publishes in immediate succession, which intermittently made the second
  one fail on real hardware even without a broken socket. Fixed by always
  deferring the terminal to the next `processPending()` call/loop
  iteration, so every call site attempts at most one response publish. This
  is now **bench-confirmed** too: a subsequent real-hardware run showed the
  terminal recovering normally on the deferred iteration when nothing failed
  (no `AWS IoT publish failed` log at all - just an intentional one-loop
  defer), and recovering ACCEPTED-then-terminal in order when the ACCEPTED
  publish genuinely failed (`mqtt_err=-9` observed). Diagnostic wording was
  further split (`TerminalDeferred` / `TerminalQueuedBehindAccepted` /
  `TerminalPublishFailed` / `TerminalPublished`) so a deferred-but-not-yet-
  attempted terminal is never logged as "publish failed" - see
  docs/mqtt-dev-smoke-test.md's outbox section.
- `DevMqttSmokeState::update()`'s `WaitingForTime` branch could reissue
  `configTime()` before a previous SNTP attempt (asynchronous, unlike the
  synchronous DNS lookup) had a chance to complete, once the exponential
  backoff interval became shorter than a real sync round trip - a plausible
  explanation for "time sync requested" repeating for a long time on one
  real boot. A first fix gated this on the caller's
  `sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS`; real hardware
  still showed several `time sync requested` lines before sync completed
  (recovering within 30 s regardless), showing that status is not reliable
  as the sole guard - it can apparently stay reset/idle for a while after
  `configTime()` is called. Replaced with a self-contained, bounded,
  configurable in-flight timer entirely inside `DevMqttSmokeState` (no
  external status consulted at all) - see docs/mqtt-dev-smoke-test.md > Real
  bench observation for the full mechanism and its tests. Whether this
  fully eliminates the repeated log lines on real hardware has not been
  re-validated yet (only `pio run`/native tests so far for this specific
  change). The ~78 s of DNS failures on that same boot was not changed - it
  is consistent with ordinary bounded exponential backoff over a
  slow/unavailable resolver path, not a code defect that was found.
- A real bench device stayed online ~110 minutes, then lost MQTT/TLS
  (`start_ssl_client: -1`) and stayed disconnected over an hour; after
  reattaching the monitor, Wi-Fi reasons 201 (`no_ap_found`)/39 (`timeout`)
  appeared, Wi-Fi/IP/first-DNS recovered, but the first TLS connect failed
  with socket `errno 113` and later attempts failed repeatedly in
  `hostByName()` while the state machine's stage stayed `mqtt`, never
  returning to `dns`. **Not attributed to AWS** - the symptoms point at a
  local Wi-Fi/DNS-path issue, and the exact cause of that local degradation
  was not diagnosed; **local Wi-Fi is not claimed to be "fixed"** by the
  response below. Two gaps addressed, both in `DevMqttSmokeState`/
  `mqtt_smoke_main.cpp` only: (1) DNS was only ever resolved once before the
  initial NTP sync - every later `ConnectMqtt` went straight to
  `transport.connect()`, whose TLS client resolves internally with no way
  for the state machine to attribute a DNS-specific failure or return to the
  `dns` stage; fixed with an explicit sanitized DNS preflight before every
  `ConnectMqtt`, falling back to `WaitingForDns` (that stage's own backoff)
  on failure via the new `networkPreflightFailed()`. (2) there was no
  autonomous recovery for "Wi-Fi says connected but DNS/TLS doesn't actually
  work" over an extended period; `DevMqttSmokeState` now counts consecutive
  DNS-preflight/TLS connectivity failures (never publish failures - those
  already have their own outbox flow) and authorizes one conservative,
  cooldown-limited (`wifiRecoveryThreshold`=3, `wifiRecoveryCooldownMs`=10
  min, both configurable) `WiFi.disconnect(false, false)` (radio on,
  credentials kept) + re-association via the existing unchanged
  `ConnectWifi`/backoff flow, never `ESP.restart()`. Full detail, wire-format
  of the new sanitized log lines, and the native test list are in
  docs/mqtt-dev-smoke-test.md > "Real bench observation: MQTT/TLS lost after
  ~110 minutes online". Validated by native tests and both firmware builds
  compiling; **not yet re-validated on real hardware**.
- Code review on the above (before it reached real hardware) found that
  `WiFi.disconnect(false, false)` is asynchronous: the caller's very next
  `wifiConnected` read can still (falsely) report `true`, and the recovery
  cascade could resume DNS/MQTT over the stale association without
  `ConnectWifi`/`WiFi.begin()` ever firing. Fixed with an explicit
  `awaitingWifiRecoveryDisconnect_` flag in `DevMqttSmokeState`: while set
  and `wifiConnected` still reads `true`, `update()` returns `None` and the
  state stays at `WaitingForWifi`; only once a real `wifiConnected=false` is
  observed does the ordinary `ConnectWifi`/`ResolveDns`/... cascade resume.
  No caller (`mqtt_smoke_main.cpp`) changes needed. See
  docs/mqtt-dev-smoke-test.md > "Follow-up fix: `WiFi.disconnect()` async
  race in the recovery cascade". Validated by a real local MSVC
  compile-and-run of `test_dev_mqtt_state` (16/16) and both firmware builds
  compiling; **not yet re-validated on real hardware**.
- **Phase 3A: Si3050/Si3011-19 firmware foundation added** (hardware Rev A
  does not exist yet). `src/intercom/si3050/` models the Si3050's
  electrical bring-up sequence (`Si3050Controller`: CS deselect -> RESET
  assert -> SCLK held high -> PCLK/FSYNC start -> wait >=10 PCLK cycles ->
  RESET release -> wait the PLL settle time -> SPI permitted) behind
  narrow SPI/PCM-clock/reset/delay interfaces, plus a debounced `/RGDT`
  ring-line reader (`RingDetector`). Every timing value is cited from the
  Si3050 datasheet now checked into this repo
  (`docs/Si3050-11-18-19.pdf`) - none is guessed. Deliberately NOT
  implemented: any Si3050 control register read/write (DAA/line
  configuration), real PCM clock generation, real SPI transactions (the
  clock polarity/phase is unconfirmed), and real ring-pattern/off-hook/
  audio behavior - see docs/si3050-bringup.md for the full scope and a
  future bring-up checklist. Validated by 21 new native tests (16 + 5) and
  both firmware builds compiling; the module is not instantiated by
  either firmware path and has no dependency on `IHardwareIO`/door
  actuation.
- **Follow-up fix (same Phase 3A foundation, before merge): review found
  `Si3050Controller::initialize()` released `/RESET` and set `ready_`
  unconditionally even though `Esp32PcmClock` is a deliberate stub whose
  `isRunning()` always reports `false`, and that an invalid
  `Si3050Config` (`pclkHz=0`) would divide by zero in the timing math.**
  Fixed: `initialize()` now returns a `Si3050InitResult`
  (`Ready`/`InvalidConfig`/`ClockNotRunning`) and gates on two fail-closed
  checks before doing anything else - `pclkHz`/`fsyncHz` must both be
  non-zero (checked before touching the bus/clock or any timing math), and
  `clock.isRunning()` must report `true` right after `clock.start()`
  (checked before either PCLK-cycle or PLL-settle wait, and before
  releasing `/RESET` or setting `ready_`). Either failure actively
  (re)asserts `/RESET`, leaves `isReady()` false, and `transferRaw()`
  keeps returning `std::nullopt`; a later `initialize()` call retries the
  whole sequence rather than being treated as a no-op. Because
  `Esp32PcmClock::isRunning()` always returns `false`, this means the
  controller now structurally cannot finish bring-up against the real
  (stub) clock implementation - a future integration cannot pick this up
  and have it silently appear to work. 4 new native tests added (clock
  never running, `pclkHz=0`, `fsyncHz=0`, retry-succeeds-once-fixed);
  total for this suite is now 16. **Not validated on any real hardware -
  Rev A does not
  exist yet.**
- **Phase 3B.1: Si3050 clock probe bench experiment added** (isolated from
  the Si3050 foundation and from all product firmware paths). Two new
  PlatformIO environments, each with an exclusive `build_src_filter`:
  `esp32-c3-si3050-clock-probe` generates the Si3050's target PCLK
  (2.048 MHz)/FSYNC (8 kHz) via the ESP32-C3's I2S peripheral in hardware
  TDM master mode (16 channels x 16 bits = 256 PCLK cycles/frame,
  `I2S_COMM_FORMAT_STAND_PCM_SHORT` for the short frame-sync FSYNC
  pulse - confirmed against the real, installed
  framework-arduinoespressif32 3.20017.241212 headers before writing any
  code, not assumed); `esp32dev-si3050-clock-meter` measures those clocks
  on a classic ESP32 DevKitV1 via hardware PCNT pulse counting (never
  `digitalRead()`/GPIO interrupts for the pulses themselves), with
  overflow-safe accumulation (the PCNT hardware register is only 16-bit)
  and real (`esp_timer_get_time()`-measured) window durations. Neither
  environment touches `Si3050Controller`, `Esp32PcmClock` (still an
  untouched stub), Wi-Fi, or any door/GPIO/relay/RGDT/SPI/reset logic for
  a real Si3050; neither is reachable from `esp32-c3`/`esp32-c3-dev-mqtt`.
  The conversion/aggregation math (`src/dev/si3050_clock_probe_math.{h,
  cpp}`) is hardware-independent and covered by 7 native tests -
  required adding `-DUNITY_INCLUDE_DOUBLE` to `[env:native]` (the first
  suite in this repo needing floating-point assertions). See
  docs/si3050-clock-probe.md for the full contract, wiring, and expected
  output. This probe does not make the product's PCM clock functional and
  does not validate the Si3050 board itself.
- **Follow-up fix (same Phase 3B.1 experiment): a real first DevKitV1
  boot found the meter's PCNT bring-up broken.** The original code
  installed `pcnt_isr_service_install()` before either PCNT unit had been
  configured; the driver rejected it ("PCNT driver error"), both
  `pcnt_isr_handler_add()` calls then failed too ("ISR service is not
  installed"), and the firmware went on to print `pcnt configured`
  anyway since no return value was checked - it would have reported
  fabricated measurements from a counter with no overflow ISR to catch
  its 16-bit wrap. Fixed by reordering to configure/pause/clear both
  units, then install the ISR service, then add handlers, then enable
  events, then resume (`src/dev/si3050_clock_probe_meter_main.cpp`), and
  by checking every relevant call's `esp_err_t` via the new
  `PcntBringupTracker` (`src/dev/si3050_clock_probe_meter_bringup.{h,
  cpp}`) - `pcnt_isr_service_install()`'s documented
  `ESP_ERR_INVALID_STATE` ("already installed") is the one code treated
  as ready to proceed; any other failure stops bring-up immediately,
  prints one sanitized `pcnt bringup failed step=... esp_err=...` line,
  and leaves `meter_started=false` (`loop()` never reports a measurement
  in that state). 6 new native tests for the tracker/decision logic
  itself (not the real PCNT calls, which native tests cannot exercise).
  See docs/si3050-clock-probe.md's "Real bench observation: PCNT bring-up
  order bug". **This fix is now confirmed on real hardware** - a physical
  retest (all three wires connected) printed `pcnt configured` and
  reported a real, stable signal.
- **Follow-up finding (same Phase 3B.1 experiment, same retest): the
  generator does not deliver the requested clock geometry.** With the
  PCNT fix confirmed, the same retest measured the generator's actual
  output for the first time: `pclk_hz~=1,024,127`, `fsync_hz~=16,003`,
  `ratio~=63.996` - not the requested `pclk_hz=2,048,000`,
  `fsync_hz=8,000`, `ratio=256` (FSYNC measured ~2x the requested sample
  rate, PCLK ~1/2 the requested value, ratio ~1/4 the requested value).
  The signal is real and stable; the earlier assumption that
  `I2S_CHANNEL_FMT_MULTIPLE` + `total_chan=16` + 16-bit samples +
  `I2S_COMM_FORMAT_STAND_PCM_SHORT` would automatically deliver a 256:1
  ratio on this driver/chip combination is falsified and has been removed
  from docs/si3050-clock-probe.md. Investigated headers-only (this
  framework ships `driver/i2s.h`'s legacy I2S driver as a precompiled
  `libdriver.a`, no source): `hal/i2s_hal.h` has a distinct `active_chan`
  field the public `i2s_config_t` cannot set directly, and
  `i2s_set_clk()`'s documented `ch` parameter ("specific channel in TDM
  mode") appears to be the driver's dedicated way to (re)apply the TDM
  channel count for clock configuration - `i2s_driver_install()` alone
  may not fully resolve it. Added an explicit, driver-justified (not
  invented) `i2s_set_clk()` call requesting the same total_chan=16/
  bits=16 via that dedicated API, plus expanded sanitized diagnostics
  that report only what was *requested* (`requested_*` fields, computed
  via the new, natively-tested `configuredTdmRatio()`/`configuredBclkHz()`
  in `src/dev/si3050_clock_probe_generator_config.h`) and never claim a
  real frequency without external measurement.
- **Follow-up (same experiment): the `i2s_set_clk()` attempt was
  physically retested twice and FAILED - removed.** Both reflashes of
  the generator measured an identical, still-wrong result
  (`pclk_hz~=1,024,100`, `fsync_hz~=16,003`, `ratio~=63.99`) to the
  original measurement above - the call changed nothing and has been
  removed from `src/dev/si3050_clock_probe_generator_main.cpp`; it must
  not be reintroduced or presented as a fix. Separately confirmed (not
  assumed from online docs) that this installed framework
  (`framework-arduinoespressif32` 3.20017.241212+sha.dcc1105b) is built
  on **ESP-IDF 4.4.7** (`esp_idf_version.h`), and that none of the newer
  native TDM driver headers (`driver/i2s_std.h`, `driver/i2s_tdm.h`,
  `driver/i2s_pdm.h`, any `esp_driver_i2s` component) exist anywhere in
  this framework package for any chip - only the legacy `driver/i2s.h`
  already in use. This is a genuine toolchain/framework-version blocker,
  not something fixable in firmware code alone; per explicit instruction,
  no further attempt was made with the legacy driver, private registers,
  LEDC, RMT, bit-banging, or delay loops. Two minimal alternatives (move
  to an ESP-IDF >= 5.0-based core, or reconsider whether this exact
  2.048 MHz/8 kHz/256:1 relationship must come from this specific
  peripheral) are documented in docs/si3050-clock-probe.md's "Real bench
  observation: generator does not reach the target ratio" for a future
  team decision. **This PR stays as investigation/documentation - it does
  not claim the ESP32-C3 delivers the Si3050's target clock on this
  toolchain, and does not decide on an external oscillator.**
- **Follow-up correction (same Phase 3B.1 experiment): the "2.048 MHz/
  8 kHz/256" target used throughout the entries above was wrong.** A
  full read of the Si3050 datasheet's Clock Generation, Communication
  Interface Mode Selection, and PCM Highway sections (cited by title,
  not section number - numbering varies between datasheet revisions)
  shows the part has two distinct modes: **GCI mode**, which does
  require PCLK = 2.048 or 4.096 MHz and multiplexes control with data;
  and **PCM/SPI mode** (SPI for control, PCM for audio - the mode
  InterBridge plans to use), where **PCLK = 1.024 MHz is a valid rate**
  (1,024,000 / 8,000 = 128 PCLK cycles/frame = 16 timeslots of 8 bits).
  The corrected target for this probe is therefore `PCLK ~= 1.024 MHz`/
  `FSYNC ~= 8 kHz`/`ratio ~= 128`, not 2.048 MHz/256. Separately, a
  hypothesis that the meter's ~16 kHz FSYNC reading (from the retest
  above) was a PCNT double-edge-counting artifact was checked against
  the actual code in `src/dev/si3050_clock_probe_meter_main.cpp` and
  found **not** to hold: both the PCLK and FSYNC PCNT units are
  configured by the same `configurePcntUnit()` function
  (`pos_mode=PCNT_COUNT_INC`/`neg_mode=PCNT_COUNT_DIS` on both), so both
  already counted rising edges only, before and after this change. This
  change renames the meter's log/struct fields for clarity
  (`pclk_edges`/`fsync_edges` -> `pclk_rising_edges`/
  `fsync_rising_edges`), adds a boot-time line stating the configured
  edge mode explicitly, adds `kPcmSpiTargetPclkHz`/
  `kPcmSpiTargetFsyncHz`/`kPcmSpiTargetRatio` constants and matching
  native tests in `src/dev/si3050_clock_probe_math.{h,cpp}`, and
  corrects this document plus docs/si3050-clock-probe.md and README.md.
  **It does not change the meter's PCNT edge configuration, the
  generator, or any production/Si3050Controller/DEV-MQTT code, and no
  real Si3050 hardware has been initialized at any point.** The ~16 kHz
  FSYNC reading remains unexplained and requires a fresh physical
  retest against the corrected target before clock compatibility can be
  marked as physically confirmed - see docs/si3050-clock-probe.md's
  "Corrected premise" and "Real bench observation: meter edge
  configuration re-examined" sections. This supersedes the abandoned
  PR #17 (which pursued an ESP-IDF 5/native TDM driver environment based
  on the same wrong 2.048 MHz/256 premise); PR #17 was closed without
  merge and none of its changes are included here.
- **Follow-up (same PR #18, second update): the ~16 kHz FSYNC/~64:1
  ratio reading is now confirmed real by a fresh physical retest**, run
  with the renamed meter fields from the entry above (`pclk_rising_edges`
  / `fsync_rising_edges`, both explicitly counting rising edges only):
  `pclk_hz~=1,024,129`, `fsync_hz~=16,004`, `ratio~=63.992` - reproducing
  the earlier reading and confirming it is a genuine, rising-edge-only
  physical measurement, not a meter artifact. This PR then investigated
  the legacy I2S generator driver in depth: it cross-referenced the
  matching upstream `espressif/esp-idf` `v4.4.7` tag's
  `components/driver/i2s.c` and `components/hal/i2s_hal.c` (not present
  as source in the installed framework package, which ships only a
  precompiled `libdriver.a`) and confirmed that `total_chan`/`chan_mask`
  genuinely reach the TDM hardware registers (`i2s_ll_tx_set_chan_num()`/
  `i2s_ll_tx_set_active_chan_mask()`), but that the driver's own
  documented clock-divider formula, evaluated for the generator's actual
  request, predicts `bclk~=2,051,282 Hz` (close to the original
  2.048 MHz request) - not the ~1.024 MHz that real hardware measures.
  Since the driver's own formula does not predict the real, confirmed
  measurement even for the *current* configuration, no other
  `total_chan`/`bits_per_sample` combination computed from that same
  formula could be trusted to reach the corrected 1.024 MHz/8 kHz/128
  target without another physical reflash/remeasure cycle - so, per
  explicit instruction not to implement an unconfirmed approximation,
  **the generator's actual I2S configuration is unchanged in this PR.**
  Only its diagnostic log field names were clarified
  (`requested_total_chan`/`requested_bits_per_sample`/`requested_ratio`
  -> `slot_count`/`slot_width_bits`/`requested_clocks_per_frame`, plus a
  new explicit `requested_fsync_hz`), and two native tests were added
  documenting the corrected target's pure math (16 timeslots x 8 bits =
  128 PCLK cycles/frame at 1.024 MHz/8 kHz) without claiming the
  generator was reconfigured to it. See docs/si3050-clock-probe.md's
  "Deeper investigation" section for the full source-grounded trace,
  including one concrete but unconfirmed candidate mechanism
  (`i2s_hal_tx_set_channel_style()`'s unconditional `half_sample_bits =
  chan_num * chan_bits / 2` register write, applied even for TDM +
  `I2S_COMM_FORMAT_STAND_PCM_SHORT`) that could not be verified without
  ESP32-C3 Technical Reference Manual register-behavior documentation
  not present in this installed framework. The meter, production,
  DEV-MQTT, and Si3050 foundation code remain untouched; no real Si3050
  hardware has been initialized at any point.
- **Follow-up (same PR #18, third update): an explicit, reversible bench
  experiment on the generator.** The prior entry concluded the legacy
  driver's clock formula couldn't reliably predict a fix, and stopped
  short of changing the generator. This update changes it anyway - not
  by trusting that same unreliable formula for a *derived* configuration,
  but by requesting the Si3050 datasheet's *own* PCM/SPI-mode PCM Highway
  geometry directly: `src/dev/si3050_clock_probe_generator_main.cpp`'s
  `kBitsPerSample` changed from `I2S_BITS_PER_SAMPLE_16BIT` to
  `I2S_BITS_PER_SAMPLE_8BIT` (16 TDM slots x 8 bits = 128 requested
  clocks/frame, was 16 x 16 = 256), confirmed accepted by the installed
  driver's own validation (`bits_per_sample % 8 == 0 && <= 32`) and by a
  clean `pio run -e esp32-c3-si3050-clock-probe` build. `total_chan=16`,
  `sample_rate=8000`, PCM-short format, master TX, and the GPIO0/GPIO1
  pin routing are otherwise unchanged; `bits_per_chan` stays at its
  driver-documented default (equal to `bits_per_sample`), and the DMA
  buffer size comment/math were updated for the 8-bit width so no stale
  16-bit assumption remains anywhere in the file. The generator's log
  now reports `requested_pclk_hz=1024000`/`requested_clocks_per_frame=
  128`/`slot_width_bits=8`. Two native tests
  (`test_si3050_clock_probe_generator_config`) assert this new requested
  geometry's pure math (`configuredTdmRatio(16, 8) == 128`,
  `configuredBclkHz(8000, 16, 8) == 1,024,000`); the previous 16 x 16
  geometry's math is kept as a separate regression-only test pair, since
  the generator no longer requests it. At the time of this update, this
  had not yet been flashed or measured on real hardware - see the
  follow-up entry immediately below for the physical confirmation. The
  meter, production, DEV-MQTT, and Si3050 foundation code remain
  untouched; no `i2s_set_clk()`, LEDC, RMT, bit-banging, delays, or
  private registers were used; no real Si3050 hardware has been
  initialized.
- **Follow-up (same PR #18, fourth/final update): the 16 x 8 geometry is
  now physically confirmed.** The ESP32-C3 was reflashed with the 16 x 8
  generator from the entry above and measured by the DevKitV1's
  already-validated, rising-edge-only PCNT meter:
  `window_us=1000245 pclk_hz=1035308.3 fsync_hz=8089.02 ratio=127.989`,
  `window_us=1002000 pclk_hz=1024096.8 fsync_hz=8002.00 ratio=127.980`,
  `window_us=1001000 pclk_hz=1024125.9 fsync_hz=8001.00 ratio=128.000`.
  The first window contains the startup transient immediately after the
  I2S peripheral came up and is not representative; the following two
  windows stabilized at `pclk_hz ~= 1,024,097`-`1,024,126`, `fsync_hz ~=
  8,001`-`8,002`, `ratio ~= 127.98`-`128.00` - matching the corrected
  PCM/SPI target (`PCLK = 1.024 MHz`, `FSYNC = 8 kHz`, `ratio = 128`)
  within normal crystal/USB-CDC jitter. **This confirms, on real
  hardware, that the ESP32-C3 on this exact toolchain and legacy I2S
  driver can generate a PCM/SPI-mode-compatible Si3050 clock signal via
  a 16 x 8 TDM configuration**, measured externally by the independent
  DevKitV1/PCNT meter board (never self-measured by the generator).
  This confirms the clock signal only: no real Si3050 or Si3011/18/19
  part has been connected or initialized at any point, PCM audio data
  (`DRX`/`DTX`) and audio content remain completely untested, and this
  validated 16 x 8 configuration exists only in the isolated
  `esp32-c3-si3050-clock-probe` bench environment - it has **not** been
  integrated into `Esp32PcmClock` (still the untouched, unintegrated
  stub) or `Si3050Controller`, and no production/DEV-MQTT/Wi-Fi/BLE/AWS
  firmware path uses it. No code changes were made in this update beyond
  documentation (this file, README.md, docs/si3050-clock-probe.md) and
  removing now-inaccurate "experimental"/"unconfirmed"/"not yet tested"
  language from source comments describing the 16 x 8 geometry
  specifically - the 16 x 8 configuration itself, the meter, and all
  other code are unchanged from the previous entry. See
  docs/si3050-clock-probe.md's "Real bench observation: 16 x 8 slot
  geometry reaches the PCM/SPI target" for the full record.
- **Phase 3B.2: PCM clock generation implemented for real and integrated
  into the normal firmware.** `Esp32PcmClock`
  (`src/intercom/si3050/si3050_pcm_clock.{h,cpp}`) is no longer a stub:
  it configures the ESP32-C3's I2S peripheral for the exact 16 x 8 TDM/
  PCM-short/1.024 MHz-PCLK/8 kHz-FSYNC geometry the Phase 3B.1 probe
  physically validated, restated as its own constants
  (`kSi3050PcmTdmSlotCount`/`kSi3050PcmTdmSlotWidthBits`) rather than a
  dependency on the probe's `src/dev/` module. Every driver call's
  `esp_err_t` is checked via a new `Si3050PcmClockBringup` (mirrors
  `PcntBringupTracker`'s role for the clock probe meter): `start()` is
  idempotent (a second call while already running never re-installs),
  any mid-sequence failure rolls back exactly what that call acquired
  (`i2s_driver_uninstall()` only if `i2s_driver_install()` actually
  succeeded), and `stop()` is safe to call repeatedly or without a prior
  `start()`. A fail-closed configuration gate
  (`si3050PcmConfigurationSupported()`) rejects any `pclkHz` that does
  not match what the fixed 16 x 8 geometry implies for the requested
  `fsyncHz`, rather than silently substituting a different value.
  `Si3050Config`'s default `pclkHz` changed from `2048000` to `1024000`
  to match. `src/main.cpp` now constructs `Esp32Si3050Bus`,
  `Esp32PcmClock`, `Esp32Si3050Reset`, `Esp32Si3050Delay`, and
  `Si3050Controller` inside a new `initializeSi3050()` (called from
  `setup()`, after `initializeHardware()`) and calls
  `si3050Controller->initialize()` once at every boot, unconditionally -
  no feature flag, since the sequence is safe with no Si3050 physically
  attached (plain GPIO/I2S operations, no register access). These are
  held in `std::optional<T>` and `.emplace()`d inside that function
  rather than as global objects like the other hardware singletons in
  that file, because `Esp32Si3050Bus`'s/`Esp32Si3050Reset`'s
  constructors call real `pinMode()`/`digitalWrite()` under `#ifdef
  ARDUINO` - which must not run before the Arduino runtime itself has
  initialized (before `setup()` starts); no currently-instantiated
  global hardware object in that file touched a real pin from a global
  constructor before this, so this would have been a genuine new boot
  risk without the deferred-construction fix. GPIO0/GPIO1 (the same pins
  the physical probe validated) come from the existing `si3050_pins.h`
  source of truth - no pin was invented or guessed. 7 new native tests
  (`test/test_si3050_pcm_clock/`) cover the TDM geometry math, the
  configuration gate, and `Si3050PcmClockBringup`'s bring-up/rollback/
  idempotency decision logic exhaustively (success, failure at each of
  the three steps with correct rollback-owed state, first-failure-wins,
  rollback-enables-retry, repeated stop() safety) - the real ESP-IDF I2S
  calls themselves are not exercised natively, same limitation as the
  clock probe meter's own tracker tests. **What this does and does not
  prove is kept explicit in docs/si3050-bringup.md's new "PCM clock:
  validation status" section**. Three validation levels are recorded:
  (1) the `16 x 8` geometry was physically validated earlier by the
  isolated probe; (2) the real `Esp32PcmClock` implementation in the
  normal `esp32-c3` environment has now also been reflashed and measured
  physically at approximately `1.024 MHz` PCLK / `8 kHz` FSYNC / `128`;
  and (3) no real Si3050 has been connected or initialized at any point,
  so `Esp32Si3050Bus::transfer()` (real SPI),
  DAA/register configuration, DRX/DTX, audio, ring, off-hook, and relay
  behavior all remain outside scope and unvalidated. The existing CI
  result remains unchanged, as do Wi-Fi/BLE/MQTT/AWS/provisioning/reconnection, the
  clock probe environments (kept as bench regression), and PR #17's IDF5
  approach (not reintroduced).
- **Phase 3B.8: bench-only DEV physical ring simulator added** (a
  momentary button on the ESP32-C3 publishes a real `RING_DETECTED`
  `DeviceEvent` through the existing production AWS IoT Basic Ingest
  pipeline, for bench-testing the downstream notification pipeline
  without a real Si3050/intercom line - see `docs/dev-ring-simulator.md`
  and `docs/roadmap-3b.md`). A new isolated environment,
  `esp32-c3-dev-ring-simulator`, gated behind
  `INTERBRIDGE_DEV_RING_SIMULATOR`, mutually exclusive (via
  `build_src_filter`) with `esp32-c3`, `esp32-c3-dev-mqtt`, and both
  Si3050 clock probe environments. It reuses `DevMqttSmokeState`
  (`src/dev/mqtt_smoke_state.*`, already bench-validated - see
  `docs/mqtt-dev-smoke-test.md`) for Wi-Fi/DNS/NTP/MQTT connectivity
  bring-up rather than duplicating it, and publishes exclusively through
  `MqttTopics::eventsIngest()` + `Esp32AwsIotTransport` +
  `MemoryEventOutbox` (the same production `IEventOutbox`
  contract) - no second MQTT client, no parallel topic. Two new
  hardware-independent, natively-tested classes: `DevRingButtonController`
  (`src/dev/dev_ring_button.*` - debounces a momentary button and emits a
  one-shot "ring requested" pulse only on the released-to-pressed edge,
  never while held, with an additional short post-event lockout against
  contact-bounce bursts) and `DevRingEventCoordinator` +
  `publishPendingEvents()` (`src/dev/dev_ring_event.*` - builds the
  `DeviceEvent`/`event_id` and enqueues it, then drains the outbox against
  the transport exactly like `main.cpp`'s `updateNetwork()` loop, so a
  retry or an offline press never regenerates the `event_id`/payload).
  Never touches the real Si3050 driver stack, `RingDetector`, PCM clock,
  provisioning, BLE, or production Wi-Fi/AWS credentials/composition -
  `main.cpp` is completely excluded from this environment's build.
  GPIO20 was chosen for the button (`src/dev/dev_ring_simulator_config.h`,
  with compile-time `static_assert`s against colliding with any real
  Si3050/BOOT/USB pin): the validated bench board only exposes 15 GPIOs
  total, and GPIO0-8/10/9/18/19 are already committed to
  Si3050/BOOT/USB - GPIO20 is a **documentation-reserved-only, currently
  unimplemented** pin (`kSi3050ReservedPinButton`; `Esp32ButtonInput` has
  no real GPIO wired in any code path yet), and its reuse here is an
  explicit, user-approved, DEV-only-scoped decision, not a silent
  conflict - see `docs/dev-ring-simulator.md` for the full pin-budget
  rationale and why this must be revisited once the Si3050 and the final
  board (with its real config/reset button) are integrated together. 11
  new native tests across `test/test_dev_ring_button` and
  `test/test_dev_ring_event` (one press → exactly one event; bounce/hold
  produce no duplicates; release+press yields a new, different
  `event_id`; a failed-then-retried publish preserves the same
  `event_id`/payload; an offline press enqueues and a reconnect replays
  it; JSON stays contract-compatible). Validated for real in this pass
  (not just claimed): `pio run` actually succeeded (real PlatformIO +
  espressif32/riscv32-esp toolchain were available in this session) for
  `esp32-c3`, `esp32-c3-dev-mqtt`, `esp32-c3-dev-ring-simulator`,
  `esp32-c3-si3050-clock-probe`, and `esp32dev-si3050-clock-meter` - all
  five compiled cleanly with `platformio.ini` only gaining one new
  exclusion line per pre-existing environment, confirming no other build
  was altered. Native tests were also genuinely compiled **and executed**
  this pass via a locally available MSVC (VS 2022 Build Tools, `cl.exe`)
  against the real ArduinoJson v7/Unity sources already fetched into
  `.pio/libdeps/native` - all 38 native suites (295 assertions) passed,
  0 failed. **Not yet validated on real hardware** - no board has been
  flashed, no physical button wired, and the offline/reconnect replay and
  AWS IoT delivery have only been exercised through native fakes
  (`FakeDevRingButtonInput`, `FakeDeviceTransport`) - see
  `docs/dev-ring-simulator.md` > Honest status.
- **Follow-up fix (same Phase 3B.8 work, before merge): GitHub Actions CI
  caught two real defects in `test/test_dev_ring_event/test_main.cpp`
  that this session's local MSVC run had not caught.** (1) A dangling
  reference: `const std::string& json = outbox.pending()[0].eventJson;`
  bound a reference into the internal storage of a `std::vector`
  temporary returned **by value** from `IEventOutbox::pending()`; that
  temporary is destroyed at the end of its own full expression, and
  MSVC/glibc happened to disagree on how visibly that use-after-free
  manifested (MSVC silently tolerated it; libstdc++ on the Ubuntu CI
  runner did not). Fixed by storing the returned vector in a real local
  variable (`auto pending = outbox.pending();`) before indexing into it,
  everywhere the test does so. (2) The `press()`/`release()` test helpers
  called `DevRingButtonController::update()` directly instead of going
  through `DevRingEventCoordinator::update()` - the coordinator (which
  owns the actual enqueue-into-outbox logic) never saw those presses at
  all, so `outbox.pending().back()` was called against an **empty**
  outbox, which is undefined behavior for `std::deque` and is what
  actually produced CI's crash. Fixed by rewriting both helpers to drive
  `coordinator.update()` (the same path the real firmware loop uses)
  instead of the underlying button controller. While fixing this, the
  suite was also reworked per review to (a) use only `ib-`+32-lowercase-
  hex device-id fixtures, asserted valid against the real
  `isValidDeviceId()` (`provisioning/device_identity.h`) rather than
  arbitrary placeholder strings like the previous `ib-test-device`/`ib-x`,
  and (b) add an explicit `event_id` format assertion
  (`^evt-[0-9a-f]{32}$`) alongside the existing uniqueness/retry/offline-
  replay/timestamp-gating coverage. Re-validated locally (MSVC, all 38
  native suites) and by rebuilding all five embedded environments
  unchanged; **the authoritative confirmation is the next GitHub Actions
  run on this branch**, not this local run - see the PR for that run's
  actual result.
- **Follow-up finding (same Phase 3B.8 work, before merge): the first
  real-hardware boot of `esp32-c3-dev-ring-simulator` did not associate
  to Wi-Fi within 120s** (`[DEV RING] local_status=wifi wifi=down
  time=pending mqtt=down outbox_size=0 uptime_s=120`, repeated at every
  heartbeat), on the same board/network `esp32-c3-dev-mqtt` was also being
  tested against, which was assumed at the time to already connect
  successfully - **this assumption was wrong, see the next follow-up
  entry below: a retest found `esp32-c3-dev-mqtt` had not actually been
  re-confirmed on this exact bring-up path, and SSID/password/network are
  not actually ruled out.** A line-by-line comparison against
  `mqtt_smoke_main.cpp` found the connectivity **logic** already
  equivalent: both drive the identical `DevMqttSmokeState` state machine,
  call `WiFi.mode(WIFI_STA)` + `WiFi.begin()` only on its `ConnectWifi`
  action, share the same retry/backoff policy, call
  `WiFi.disconnect(false, false)` only on `RecoverWifi`, never
  `ESP.restart()`, and build with identical flags/board - no functional
  divergence in *when* Wi-Fi actions are authorized was found. What was
  genuinely missing was **observability**: the simulator had no
  `WiFi.onEvent()` handler, no boot diagnostics
  (`previous_reset=`/`resetReasonName()`), and none of
  `mqtt_smoke_main.cpp`'s per-action log lines (connect-requested,
  DNS/MQTT retry timing, state transitions, wifi-recovery-requested) - the
  single 15s heartbeat could show `wifi=down` but nothing about *why*.
  Fixed by adding that exact logging pattern (copied verbatim from
  `mqtt_smoke_main.cpp`'s own already-hardware-validated helpers -
  `onWifiEvent()`, `wifiDisconnectReasonName()`, `resetReasonName()`, and
  a log line per `DevSmokeAction`) into `dev_ring_simulator_main.cpp`
  itself, without modifying `mqtt_smoke_main.cpp` at all (it is already
  validated on real hardware; duplicating a few small, pure diagnostic
  helpers is lower-risk than editing it). **This closes an observability
  gap, not a proven functional bug** - no speculative fix (e.g.
  reassigning GPIO20) was made without hardware evidence tying it to the
  failure; see `docs/dev-ring-simulator.md` > "Real bench observation:
  first boot never associated with Wi-Fi" for the full reasoning. A
  hardware retest with these diagnostics is still required to see the
  actual disconnect reason (or confirm/rule out a silent reset loop via a
  changing `previous_reset=` value) - **still not validated on real
  hardware**; button behavior, MQTT connectivity, and end-to-end delivery
  from a real press remain unexercised on hardware.
- **Follow-up fix (same Phase 3B.8 work, before merge): the hardware
  retest with the diagnostics above revealed a real, shared coordination
  defect, not just a missing-observability problem.** Both
  `esp32-c3-dev-ring-simulator` and `esp32-c3-dev-mqtt` showed the
  identical Wi-Fi association failure on the same session
  (`wifi event=disconnected reason=2`, `reason=202`, `Wi-Fi connect
  requested; ... delay_ms=16000`), and `esp32-c3-dev-mqtt`'s own serial
  log additionally showed `wifi:sta is connecting, return error` /
  `WiFiSTA.cpp begin(): connect failed!` - meaning **`esp32-c3-dev-mqtt`
  had never actually been re-confirmed working on this exact bring-up
  path**; the previous entry's "already connects successfully" framing
  was wrong, carried over from an earlier, separate bench session. The
  root cause: `DevMqttSmokeState` (`src/dev/mqtt_smoke_state.*`, shared by
  both DEV mains) modeled DNS/NTP/MQTT attempts as asynchronous
  ("in-flight" until an explicit result or timeout) but treated Wi-Fi
  association as if `!wifiConnected` alone authorized a retry once the
  ordinary backoff elapsed - with no way to distinguish "still
  associating" from "gave up." The ESP32 Wi-Fi driver actively rejects
  (and effectively restarts) a `WiFi.begin()` call issued while a previous
  attempt is still outstanding, which is exactly what the observed driver
  log lines show, and which a short-enough backoff could trigger on both
  DEV environments identically, since they share this one coordinator.
  Fixed by giving `DevMqttSmokeState` the same in-flight tracking NTP
  already has for Wi-Fi association: `ConnectWifi` is never reissued while
  `wifiAttemptInFlight()` is true; the attempt resolves only via an
  explicit `wifiAssociationResult(nowMs, success)` call (forwarded from a
  real `ARDUINO_EVENT_WIFI_STA_CONNECTED`/`GOT_IP`/`DISCONNECTED` event) or
  a new, separate, configurable `wifiAssociationTimeoutMs` (default
  15000ms, appended as the constructor's last parameter so no existing
  positional call site broke); a failure or timeout schedules the next
  backoff-governed retry at resolution time, not at the moment
  `ConnectWifi` was originally issued. Both `mqtt_smoke_main.cpp` and
  `dev_ring_simulator_main.cpp` now record the real Wi-Fi event as a
  minimal `volatile` flag inside `onWifiEvent()` (which the ESP32 Arduino
  core runs on its own Wi-Fi/event task) and only turn it into an actual
  `wifiAssociationResult()` call from their own single-threaded `loop()`
  (`drainWifiEventSignals()`, called once per iteration before
  `connectivity.update()`) - never mutating the shared state machine
  directly from the event callback, which would have been a real
  concurrency hazard. The state-machine fix itself lives in exactly one
  place (`mqtt_smoke_state.*`); only this small amount of Arduino-main
  event-forwarding glue is necessarily duplicated per entry point, same as
  `stateName()`/`safeStatus()`/`NtpClock` already were. 7 new native tests
  in `test/test_dev_mqtt_state` (23 total in that suite now): `ConnectWifi`
  issued exactly once and never reissued by rapid updates while pending;
  an explicit success ends the attempt and the ordinary cascade still
  advances normally; an explicit failure ends the attempt and schedules
  the next retry no earlier than its own backoff deadline; a stuck attempt
  with no event at all is abandoned by its own timeout and allows a later
  retry; that timeout is wraparound-safe; the existing `RecoverWifi`
  ladder's own reconnect attempt is equally protected from reissue; and a
  burst of repeated failure signals for the same attempt never causes more
  than one `WiFi.begin()`. One pre-existing test
  (`test_backoff_is_capped_and_deadline_wrap_is_safe`) needed updating to
  call the new `wifiAssociationResult()` explicitly between attempts,
  since Wi-Fi retries are no longer resolved implicitly/instantly the way
  they were before this fix - every other pre-existing test in that suite
  was unaffected (all of them already transition `wifiConnected` to `true`
  on the very next call after issuing `ConnectWifi`, which bypasses the
  new in-flight gate entirely, exactly as intended). Validated in this
  pass: all 38 native suites (309 assertions) compiled and executed via
  MSVC, 0 failed; `esp32-c3`, `esp32-c3-dev-mqtt`,
  `esp32-c3-dev-ring-simulator`, `esp32-c3-si3050-clock-probe`, and
  `esp32dev-si3050-clock-meter` all compiled with `pio run` (real
  espressif32/riscv32-esp toolchain). **This fixes the concurrent-retry
  coordination bug; it does not by itself prove Wi-Fi will now associate**
  - disconnect reasons 2 (auth expire) and 202 can also indicate a
  credential/AP-side/signal problem unrelated to this bug, and that must
  be re-evaluated on a fresh hardware retest, not assumed either way from
  the evidence gathered so far. GPIO20 remains unreassigned - no evidence
  ties it to this specific failure. **Still not validated on real
  hardware**; see `docs/dev-ring-simulator.md`'s "Real bench observation:
  retest reveals a shared concurrent-retry defect" and Honest status.
- **Follow-up finding + fix (same Phase 3B.8 work, before merge): a third
  hardware retest confirmed the concurrent-retry fix worked** - the
  driver-level `wifi:sta is connecting, return error` error did not
  recur. **Wi-Fi association still failed, with the same disconnect
  reasons (2, 202) as before that fix** - meaning the concurrent retry was
  not the (sole) cause of the original failure. Reason 2/202 has not been
  root-caused yet; it will be isolated next using a dedicated WPA2 test
  hotspot with known-good credentials, separate from whatever network the
  board has been tested against so far. SSID/credential/network causes
  remain explicitly not ruled out - only the concurrent-retry coordination
  defect is fixed. The same retest also surfaced an unrelated,
  diagnostic-only bug: "delay_ms=" log lines showed impossible values
  (`4294967291`, `4294967294`) from computing `retryAtMs() - now` via
  plain `uint32_t` subtraction, which underflows whenever the deadline is
  at or past `now` - guaranteed by construction the instant `ConnectWifi`
  fires (`deadlineReached()` must already be true then). Fixed with a new
  `DevMqttSmokeState::millisUntil(deadlineMs, nowMs)` static helper
  (saturates at 0, wrap-safe via the same signed-subtraction technique as
  `deadlineReached()`), used at all 8 "delay_ms=" call sites across both
  DEV mains. **This is display-only - no retry/backoff policy changed**,
  per explicit instruction not to alter retry policy based on a logging
  artifact alone. 1 new native test
  (`test_millis_until_saturates_and_is_wrap_safe` in
  `test/test_dev_mqtt_state`, 24 tests total in that suite now): ordinary
  future deadline, deadline already reached (saturates instead of
  underflowing), and both directions of the `millis()` wraparound.
  Validated: all 38 native suites (`pio` toolchain's own `pio test -e
  native` still cannot run natively in this sandbox - same pre-existing
  no-host-compiler limitation as every prior pass; validated instead via
  the same locally available MSVC used throughout this work) passed, 0
  failed; all five required environments (`esp32-c3`,
  `esp32-c3-dev-mqtt`, `esp32-c3-dev-ring-simulator`,
  `esp32-c3-si3050-clock-probe`, `esp32dev-si3050-clock-meter`) compiled
  via real `pio run`. **Still not validated on real hardware for
  association success** - see `docs/dev-ring-simulator.md`'s "Real bench
  observation: concurrent retry gone, auth still fails (reason=2/202)".
- **Follow-up finding + fix (same Phase 3B.8 work, before merge): a fourth
  hardware retest against a dedicated WPA2 test hotspot (an iPhone's
  Personal Hotspot, "Henrique's iPhone") produced a *different* failure
  than the home network** - `reason=201` (`WIFI_REASON_NO_AP_FOUND`,
  meaning the ESP32 never saw a beacon for that SSID at all), while the
  home network kept failing with `reason=2`/`202` (an auth-stage
  rejection) unchanged. Neither is root-caused. The credential path was
  reviewed: both DEV mains read `INTERBRIDGE_DEV_WIFI_SSID`/
  `_PASSWORD` directly from `include/interbridge_dev_secrets.h` straight
  into `WiFi.begin()` - there is no other credential source - but the
  only existing diagnostic (`wifi_config=present`) only proved *some*
  header existed, never that the compiled binary actually held the
  intended byte values. Added, sanitized, never logging the raw
  SSID/password value or any other network's name: (1) a credential
  config summary - `config=valid|invalid ssid_bytes=N password_bytes=N
  placeholder=true|false` (`valid` requires both fields non-empty and
  neither still equal to `include/interbridge_dev_secrets.example.h`'s
  placeholder text) - and (2) a controlled Wi-Fi scan summary -
  `networks_found=N configured_ssid_found=true|false rssi=N channel=N
  auth=... scan_age_ms=N`, where RSSI/channel/auth are only ever the
  configured SSID's own values if present; no other network's SSID is
  ever retained or logged. Per explicit follow-up instruction, both lines
  are logged not just once at boot but repeated before every
  `WiFi.begin()` and from the heartbeat while Wi-Fi is down, since the
  serial monitor is often attached only after boot and would otherwise
  miss them. The scan itself is never repeated before every attempt (a
  multi-second blocking operation via `WiFi.scanNetworks()`'s default
  synchronous mode) - only the first-ever `ConnectWifi` always scans;
  later attempts rescan only after a 5-minute interval or 5 consecutive
  explicit association failures accumulate (`kWifiRescanIntervalMs`/
  `kWifiRescanFailureThreshold`), and a scan is always fully synchronous
  with, and strictly precedes, the `WiFi.begin()` call in the same
  `ConnectWifi` handler - never concurrent with association by
  construction, and no retry/backoff policy was touched. The pure
  comparison/formatting logic
  (`diagnoseCredentialField()`/`summarizeCredentialConfig()`/
  `summarizeWifiScan()`/both line formatters) lives in a new shared,
  native-testable module, `src/dev/dev_wifi_diagnostics.*`, used
  unmodified by both `mqtt_smoke_main.cpp` and
  `dev_ring_simulator_main.cpp` - only the small Arduino-only glue
  (reading `WiFi.SSID()`/`RSSI()`/`channel()`/`encryptionType()`, calling
  `WiFi.scanNetworks()`, the `Serial.print` calls) is duplicated per main,
  same pattern as `DevMqttSmokeState`'s other callers.
  `scripts/generate_dev_secrets_header.ps1` now also rejects an SSID/
  password that still exactly matches the example header's placeholder,
  reports only the generated SSID/password byte lengths (via
  `[Text.Encoding]::UTF8.GetByteCount`, matching what the compiled C++
  macro actually holds - never `.Length`, which counts UTF-16 code units
  and would misreport a multi-byte SSID) after writing, and validates
  that all seven expected macros appear exactly once in the generated
  content before it is ever written to disk. 9 new native tests in
  `test/test_dev_wifi_diagnostics` (39 suites total now): placeholder
  detection, correct byte lengths, a config summary that is `valid` only
  when both fields are non-empty and non-placeholder, scan-summary
  found/not-found matching (including RSSI/channel/auth extraction), and
  - run through the entire pipeline with distinctive marker
  SSID/password/network-name strings - that the formatted diagnostic
  lines never contain any of those secret/name values. Validated: all 39
  native suites passed via the same locally available MSVC used
  throughout this work (`pio test -e native` itself still cannot run in
  this sandbox - same pre-existing no-host-compiler limitation); all five
  required environments compiled via real `pio run`; the PS1 script's new
  logic (placeholder rejection, UTF-8 byte counting including a
  multi-byte code point, escaping, macro-uniqueness validation) was
  exercised via a standalone throwaway harness reproducing each snippet,
  since running the real script would have overwritten the operator's
  live local DEV secrets file. **This is diagnostics only - no
  credential/AP root cause is claimed fixed or ruled out by this pass.**
  See `docs/dev-ring-simulator.md`'s "Real bench observation: WPA2 test
  hotspot isolates a different failure mode (reason=201)" and "Wi-Fi
  config and scan diagnostics" for the full record. **Still not validated
  on real hardware.**
- **Follow-up fix (same Phase 3B.8 work, before the planned hardware
  retest, before merge): a pre-retest review of the diagnostics above
  found and fixed three real defects, none yet observed on hardware.**
  (1) `DevMqttSmokeState::update()` armed the Wi-Fi association attempt's
  in-flight deadline at the moment `ConnectWifi` was issued - before the
  caller's handler ran the (possibly multi-second, blocking) diagnostic
  scan and only then called the real `WiFi.begin()`, so a slow scan could
  silently consume part of the association timeout before association
  even started. Fixed with a new `DevMqttSmokeState::wifiAssociationStarted(nowMs)`,
  called by both DEV mains with a freshly-read `millis()` immediately
  alongside the real `WiFi.begin()` (after any scan), re-arming the
  deadline from that real start time; `update()`'s own deadline at issue
  time is now explicitly documented as provisional. No change to backoff
  or to when `ConnectWifi` is authorized. (2) A failed `WiFi.scanNetworks()`
  call (negative return) was being summarized identically to a scan that
  legitimately found zero networks - both logged
  `networks_found=0 configured_ssid_found=false`, indistinguishable.
  `WifiScanSummary` now carries an explicit `status`
  (`WifiScanStatus::Success`/`Failed`) and a sanitized `errorCode`;
  `formatWifiScanLine()` prints `scan_status=failed error=N` and omits
  `configured_ssid_found`/`rssi`/`channel`/`auth` entirely rather than
  misleading zeros/false, and this status persists correctly in
  `lastWifiScanSummary` across every later repeated line (heartbeat,
  pre-`WiFi.begin()`), not just the line printed at scan time. (3)
  `diagnoseCredentialField()` took `const std::string&`, so passing the
  raw `INTERBRIDGE_DEV_WIFI_SSID`/`_PASSWORD` macros allocated a fresh
  heap copy of the secret on every call - and this runs on every
  heartbeat tick while Wi-Fi is down, for as long as the device stays
  offline. Changed to `std::string_view` (standard since C++17, which
  this project already targets) for both `value` and `placeholder` -
  passing a `const char*` literal now constructs a zero-allocation view;
  only the returned formatted line (pure metadata) is still a real
  `std::string`. 5 new native tests: 2 in `test/test_dev_mqtt_state`
  (`wifiAssociationStarted()` correctly re-arms the timeout from a
  simulated post-scan begin time, and is a no-op with nothing in flight -
  26 tests total in that suite now) and 3 in
  `test/test_dev_wifi_diagnostics` (a valid zero-network scan
  distinguishable from a failed one; the failed status/error surviving
  repeated formatting much later; `diagnoseCredentialField()` called
  directly against a `std::string_view` built from a raw `const char*` -
  a test that would fail to *compile*, not just fail an assertion, if the
  no-copy property ever regressed - 12 tests total in that suite now).
  Validated: all 39 native suites passed via the same locally available
  MSVC used throughout this work (`pio test -e native` itself still
  cannot run in this sandbox); all five required environments compiled
  via real `pio run`. **Still purely diagnostic and pre-retest - no
  credential/AP root cause is claimed fixed or ruled out, and this has
  not been flashed to real hardware.**
- **Follow-up finding + fix (same Phase 3B.8 work, before merge): a fifth
  hardware test found a real, reproducible cause of the Wi-Fi failures
  unrelated to credentials or either access point - the button's GPIO20
  wiring itself.** With the button physically disconnected from GPIO20,
  the firmware associated cleanly all the way to `Online` (Wi-Fi → DNS →
  NTP → MQTT) - the first full success in this entire investigation;
  reconnecting the button to GPIO20 disconnected Wi-Fi again. The exact
  physical mechanism is **not** established (a candidate silicon
  function on GPIO20, this specific button/wiring's mounting, a pinout
  quirk of this board sample, or electrical/RF interference are all
  still open), and this does not by itself prove GPIO20 was *also* the
  cause of the earlier reason=201/2/202 disconnects recorded above - only
  that removing it let the firmware reach `Online` at least once. The
  correlation was reproduced (disconnect → succeeds, reconnect → fails)
  and is treated as sufficient to abandon GPIO20 for this bench rig
  without waiting for a root cause. Since this board has no other free
  GPIO (see the exhaustive pin-budget accounting already in
  `dev_ring_simulator_config.h`), and GPIO21 was never itself tested but
  is avoided out of the same caution, the only remaining option is a
  deliberate overlap with one real Si3050 pin - safe here specifically
  because `esp32-c3-dev-ring-simulator` never compiles or initializes any
  Si3050 code and no Si3050 is physically attached during this bench
  test. `kDevRingButtonPin` now equals `kSi3050PinPcmDrx` (GPIO4); the
  header's `static_assert`s were changed from "not equal to any Si3050
  pin" to "equal to exactly this one approved overlap," so any future
  edit to the button pin must deliberately update that assertion too -
  it cannot silently drift to an unreviewed pin. **The real Si3050 pin
  map (`src/intercom/si3050/si3050_pins.h`) and production firmware are
  completely untouched** - this only changes which pin this DEV-only
  bench button uses. No test files needed updating (none referenced the
  GPIO number directly - they exercise `IDevRingButtonInput`/
  `DevRingEventCoordinator` through fakes, never the real pin). Docs
  (`docs/dev-ring-simulator.md`'s wiring diagram/"Why GPIO4" section and
  a new "Real bench observation: GPIO20 causes Wi-Fi to disconnect",
  `docs/roadmap-3b.md`, `README.md`) updated with this honestly: GPIO20
  disconnected → `Online`; GPIO20 reconnected → Wi-Fi dropped; cause not
  isolated; GPIO4 requires its own hardware retest before being trusted
  either. Validated: all 39 native suites passed via MSVC; all five
  required environments compiled via real `pio run`. **Still not
  validated on real hardware with the button on GPIO4.**
- **Technical closing pass (same Phase 3B.8 work, before merge): the
  GPIO20 framing above was itself corrected, the button's electrical
  interface was fixed to match the real component, the diagnostic scan
  was simplified, a presence/health signal was added, and
  `docs/dev-ring-simulator.md` was consolidated from ~670 to ~380 lines.**
  (1) **Corrected interpretation of the GPIO20 test**: the physical
  component wired to the ring-simulator button is a *Linker Button
  module* (a PCB with `VCC`/`GND`/`SIG`, active-HIGH per its own
  documentation - `SIG` reads LOW released, HIGH pressed), not a bare
  dry-contact switch. The firmware and every earlier entry above assumed
  a dry, active-LOW contact wired with `INPUT_PULLUP`. This means the
  GPIO20 test was run with an electrically mismatched wiring assumption,
  so the reproducible correlation it found (disconnected → `Online`;
  reconnected → Wi-Fi dropped) cannot be attributed to the GPIO20 pin,
  a UART silicon function, or anything else specific - only that this
  particular, mismatched assembly correlated with the drop. Every
  overclaiming statement from earlier entries ("GPIO20 causes Wi-Fi to
  disconnect", implicitly ruling GPIO21 out too, treating the credential
  as confirmed either way) is superseded by this correction. (2) **Fixed
  the electrical interface**: `Esp32DevRingButtonInput::isPressed()` now
  reads `digitalRead(kDevRingButtonPin) == HIGH` (was `== LOW`), and
  `setup()` now calls `pinMode(kDevRingButtonPin, INPUT)` (was `INPUT_
  PULLUP`) - the module drives the pin itself in both states, so no
  internal pull-up is used. The boot log line changed to `button
  initialized gpio=4 mode=INPUT active=high module=linker`.
  `DevRingButtonController` itself needed no change - it is already
  hardware-independent and only ever sees the adapter's already-resolved
  boolean, never a raw voltage level. (3) **Simplified the Wi-Fi scan to
  exactly once per boot**: a real hardware test showed
  `WiFi.scanNetworks()` itself returning `-2` after several association
  attempts, meaning the earlier interval/failure-count rescan policy
  could let the diagnostic scan interfere with the very association it
  was meant to help diagnose. Removed `kWifiRescanIntervalMs`,
  `kWifiRescanFailureThreshold`, and `consecutiveWifiAssociationFailures`
  from both DEV mains entirely (dead code once the policy was gone,
  including the incorrect earlier claim that GPIO21 was also "ruled out"
  - it was never tested at all); the `ConnectWifi` handler now gates the
  scan purely on `!wifiScanEverRun`. A manual reboot is what allows a
  fresh scan. The already-existing repeated summary lines (boot, every
  `WiFi.begin()`, heartbeat while down) are unchanged. (4) **Added a
  presence/health signal**: a real test reached `state mqtt -> online`
  (local Wi-Fi/DNS/NTP/MQTT connectivity) while the companion app still
  showed the device offline. `esp32-c3-dev-mqtt`'s existing
  `publishHealth()` contract (periodic `HealthReport` - device ID,
  firmware version, `intercom_state=Idle`, uptime, Wi-Fi RSSI, free heap
  - to `MqttTopics::healthIngest()`, QoS `AtMostOnce`, gated on Wi-Fi/
  time/MQTT validity and a 60s cadence) was confirmed in code and added
  identically to `dev_ring_simulator_main.cpp` (new `HealthReporter`
  instance, `publishHealth()` function, called once per `loop()`
  iteration, entirely independent of `eventOutbox`). **This is NOT
  independently confirmed against the actual backend/app presence
  mechanism** (outside this repo, not inspected) - only that this is the
  one periodic presence-shaped signal that already exists in this
  firmware's own contract; if a retest shows the app still doesn't
  reflect presence, the backend/app's real mechanism needs to be found in
  its own repo rather than assumed further here. (5) **Documentation
  consolidation**: `docs/dev-ring-simulator.md` was rewritten from six
  separate, partially-overlapping "Real bench observation" sections
  (~670 lines total) into a single chronological "Bench test history"
  section plus one final "Honest status" (~380 lines), preserving every
  substantive fact while removing repeated/contradictory restatements of
  the same conclusions. `README.md` and `docs/roadmap-3b.md`'s Phase
  3B.8 entries were rewritten the same way. **Consequence for this
  CONTEXT.md file**: several entries above quote specific
  `docs/dev-ring-simulator.md` sub-section titles (e.g. "Real bench
  observation: first boot never associated with Wi-Fi") that no longer
  exist verbatim after that consolidation - those entries are kept as an
  unedited historical log per this file's own convention, but readers
  following those quoted titles should go to
  `docs/dev-ring-simulator.md` > Bench test history directly instead. (6)
  Native tests: 1 new test in `test/test_dev_ring_button`
  (`test_released_low_state_produces_no_event`, plus comment updates
  reframing the existing press/hold/release-repress tests around the
  Linker Button's real LOW/HIGH semantics - `DevRingButtonController`'s
  logic itself did not change, since it was already hardware-independent).
  Validated: all native suites passed via MSVC; all five required
  environments compiled via real `pio run`, including the electrical fix,
  the scan simplification, and the new health publish. At PR #20 merge,
  this had not been validated on real hardware with the Linker Button correctly wired
  to GPIO4 at 3.3V. **That status is superseded
  by the controlled-stimulus validation below; the Linker Button itself
  remains unvalidated.**
- **Phase 3B.8 end-to-end hardware validation after PR #20:** the isolated
  `esp32-c3-dev-ring-simulator` environment was compiled and flashed to an
  ESP32-C3 Super Mini. The successful test did not use the Linker Button:
  GPIO4 was held LOW through an approximately 10 kΩ resistor to GND and
  pulsed momentarily to 3V3. Wi-Fi connected, NTP synchronized, AWS IoT
  MQTT/mTLS connected, and the health report was published; the device then
  appeared online in the app. One pulse produced exactly one `valid press
  detected; RING_DETECTED enqueued` log and exactly one `publish confirmed
  count=1 remaining=0`; the event traversed AWS IoT, `telemetry_ingestion`,
  `push_sender`, FCM, and appeared as an Android notification. **3B.8 is
  therefore complete and validated end to end on real hardware.**
  This validates the isolated DEV environment, active-high GPIO transition,
  single-event coordinator/debounce path, MQTT publish, backend/FCM delivery,
  Android presentation, and health/presence path. It does not validate the
  Linker Button electrically, held/repeated presses, physical offline replay
  or stable `event_id`, a real Si3050/line/ring source, audio/call states,
  production firmware, BLE onboarding, or the complete call UI. GPIO4 is
  still only a provisional DEV overlap with `kSi3050PinPcmDrx`, safe because
  the simulator does not compile or initialize Si3050 code and no Si3050 was
  connected. It neither changes production pinout nor permits simultaneous
  button/DRX use; the final board must decide the assignment. A future Linker
  Button evaluation is optional and separate from closing 3B.8.
- **DEV environment divergence found and fixed after the 3B.8 hardware
  validation above: `esp32-c3-dev-ring-simulator` never subscribed to the
  commands topic.** The firmware currently loaded for the 3B.8 hardware run
  was compiled from that environment; it validated Wi-Fi/NTP/MQTT/health/
  `RING_DETECTED` end to end, but a real `OPEN_DOOR` sent from the app was
  never received - `esp32-c3-dev-ring-simulator` had adopted
  `esp32-c3-dev-mqtt`'s connectivity bring-up (`DevMqttSmokeState`) but
  never its command-processing composition
  (`RemoteCommandProcessor`/`CommandHandler`/dedup/`DisabledHardware`/
  `DisabledSystemControl`), which was already real-hardware-validated on
  `esp32-c3-dev-mqtt` (`docs/mqtt-dev-smoke-test.md`). Fixed by reusing that
  exact composition unchanged - not a second implementation - shared via
  two new tiny headers (`src/dev/dev_disabled_hardware.h`,
  `src/dev/dev_command_diagnostics.h/.cpp`) so `esp32-c3-dev-mqtt` and
  `esp32-c3-dev-ring-simulator` can no longer silently diverge on the
  `DisabledHardware`/`DisabledSystemControl` guarantee again. A valid
  `OPEN_DOOR` still only ever reaches `ACCEPTED` then
  `REJECTED/CAPABILITY_DISABLED`; no door/system action is genuinely
  performed. See `docs/dev-ring-simulator.md` > "Command processing (Phase
  3B.8 cumulative pass)" for the full diagnosis, what changed, and exactly
  what still needs a real-hardware retest (this specific combination -
  ring simulator + commands - has never run on real hardware).
- **Follow-up architectural fix: the composition/cycle itself was still
  duplicated, not just the two classes above.** The fix directly above
  shared `DisabledHardware`/`DisabledSystemControl`/the diagnostic log
  wording, but `InMemoryDedupCache`, `Intercom`, `CommandHandler`,
  `RemoteCommandProcessor`, the commands-topic subscription, and the
  pending-response drain were still constructed and wired by hand,
  separately, in both `mqtt_smoke_main.cpp` and
  `dev_ring_simulator_main.cpp` - the 39 native suites existing at the time
  could not detect this because they test those classes in isolation, never
  whether a given DEV `*_main.cpp` actually builds and drives them. Fixed by
  extracting `DevCommandEnvironment`
  (`src/dev/dev_command_environment.h/.cpp`): one class that owns the whole
  composition behind a small `subscribe()`/`processPending()`/
  `setDiagnosticCallback()` surface. Both DEV entry points now construct
  exactly one `DevCommandEnvironment` and call only that surface; neither
  declares `RemoteCommandProcessor`/`CommandHandler`/`InMemoryDedupCache`
  itself anymore. Two things now make this specific gap mechanically hard to
  reintroduce, not just documented against: `scripts/check_repo_safety.py`
  greps both `*_main.cpp` files for the `DevCommandEnvironment`
  construction and the `subscribe()`/`processPending()` calls, and a new
  `test/test_dev_command_environment` suite (40th native suite) exercises
  the shared class directly - commands-topic subscription at QoS 1, a full
  `OPEN_DOOR` → `ACCEPTED` → `REJECTED/CAPABILITY_DISABLED` cycle (never
  `COMPLETED`), and re-subscription after a simulated disconnect/reconnect
  still processing a command correctly. `RING_DETECTED` event
  generation/outbox (`DevRingEventCoordinator`/`publishPendingEvents`)
  remain unchanged by this pass; what changed is that `Online` now also
  requires a successful command-topic subscription and every loop iteration
  drains pending command responses alongside the event outbox - see
  `docs/dev-ring-simulator.md` > "Command processing" for the corrected,
  precise before/after.

  **DEV environment evolution rule** (new, general going forward): the
  canonical DEV integration environment (`esp32-c3-dev-ring-simulator`, as
  the most complete one, and `esp32-c3-dev-mqtt` before it) must stay
  *cumulative* - it must preserve every previously-validated, still-
  compatible capability (Wi-Fi, NTP, AWS IoT MQTT/mTLS, health reports,
  event publishing/outbox, command subscription/processing) rather than
  silently losing one when a new capability is added. Strictly isolated,
  narrow-purpose experiments (e.g. the Si3050 clock probe pair) are exempt
  and may stay minimal on purpose - they are deliberately never meant to
  accumulate capabilities from the other DEV environments. GPIO4/
  `RING_DETECTED` remains only a temporary DEV substitute for the real
  Si3050-based ring detector, not a production mechanism.
- **Call-session extension of the 3B.8 DEV ring simulator: GPIO3 now
  simulates the end of the same simulated call (`RING_ENDED`), correlated
  with GPIO4's `RING_DETECTED` by a new, shared `call_id`.** Following the
  DEV environment evolution rule directly above, this keeps every
  previously-validated capability (Wi-Fi/NTP/MQTT, health, command
  processing via `DevCommandEnvironment`) intact and adds a minimal
  `Idle`/`Ringing` state machine on top, owned by (the now two-button)
  `DevRingEventCoordinator` (`src/dev/dev_ring_event.h/.cpp`): a GPIO4
  pulse in `Idle` generates a new `call_id` and enqueues `RING_DETECTED`;
  a GPIO3 pulse in `Ringing`, or a DEV-only safety timeout
  (`kDevCallTimeoutMs`, 60s default, aligned with the app's own
  ring-timeout fallback) if GPIO3 never pulses, enqueues `RING_ENDED`
  reusing that exact `call_id` and returns to `Idle`. A GPIO3 pulse with
  no active call, and a second GPIO4 pulse while already `Ringing`, are
  both explicitly ignored (no new event, no state change) rather than
  silently mis-tracked. `protocol/messages.h/.cpp` gained
  `ProtocolEventName::RingEnded` and an optional `DeviceEvent::callId`
  (omitted from the JSON, and therefore fully retrocompatible, for every
  event that isn't part of a call session);
  `docs/communication-protocol.md` section 16.1 records the `RING_ENDED`/
  `call_id` wire contract (at the time of this specific bullet, only a
  firmware-side proposal - later coordinated with the backend and app,
  see the follow-up bullet below).
  GPIO3 is a second, equally provisional DEV-only overlap - this time
  with `kSi3050PinPcmDtx` (DTX) - justified for exactly the same reason
  as GPIO4/DRX (`esp32-c3-dev-ring-simulator` never compiles or
  initializes Si3050 code, and no Si3050 is attached while it runs), with
  the same compile-time-asserted single-approved-pin guard in
  `dev_ring_simulator_config.h`. A genuine, previously-latent ordering bug
  was found and fixed while implementing this: `publishPendingEvents()`
  (`src/dev/dev_ring_event.cpp`) used to keep iterating past a failed
  publish, which could have let a later-enqueued `RING_ENDED` publish
  successfully ahead of an earlier-enqueued `RING_DETECTED` still stuck at
  the front of the outbox after a failed retry attempt for it specifically
  - it now stops at the first failed publish, preserving strict FIFO
  publish order. Proven by 16 native tests in a rewritten
  `test/test_dev_ring_event` (state machine transitions/ignored edges,
  `event_id`/`call_id` distinctness and reuse, debounce on both GPIOs, the
  GPIO3-vs-timeout race producing only one `RING_ENDED`, the publish-order
  fix, and restart always starting `Idle`) - all pre-existing native
  suites are unaffected. At the time of this specific bullet, not yet
  exercised on real hardware; a later pass closed that gap - see
  `docs/dev-ring-simulator.md` > "Call session addition
  (GPIO3/`RING_ENDED`/`call_id`): hardware-validated, integrated with
  backend and app" for the real-hardware record, and its "Call session
  manual test procedure" for exactly which steps that run exercised. This
  does not claim the Si3050 was tested, that a real intercom line's ring
  end was physically detected, that the Linker Button was validated, or
  that GPIO3/GPIO4 are final production pins.
- **Real-hardware finding + fix: `esp32-c3-dev-ring-simulator` connected
  successfully, then lost connectivity and never recovered without a
  manual reboot.** Two independent, compounding defects in this
  repository's own code (not the AP/router): (1)
  `ARDUINO_EVENT_WIFI_STA_CONNECTED` (L2 association only, before DHCP)
  was forwarded to `DevMqttSmokeState` as a full success signal
  identically to `..._GOT_IP` - combined with a latent bug in
  `DevMqttSmokeState` itself (a success signal cleared the in-flight
  attempt immediately with no further safeguard), this could permanently
  strand the state machine in `WaitingForWifi` with `actionIssued_` stuck
  `true` and no pending transition to ever reset it, if `wifiConnected`
  then never actually became true - exactly what a real bench log showed.
  `reason=8` (`WIFI_REASON_ASSOC_LEAVE`) disconnects were also ambiguous:
  the driver reports that same code for both an AP-initiated drop and this
  firmware's own `RecoverWifi`-triggered `WiFi.disconnect()`. (2) The
  ESP32 Arduino core's own Wi-Fi auto-reconnect (on by default) was never
  disabled, letting it retry association internally, racing against
  `DevMqttSmokeState`'s own explicit `ConnectWifi`/backoff cadence as a
  second, uncoordinated source of connect/disconnect events. **Fixed**:
  `DevMqttSmokeState` (`src/dev/mqtt_smoke_state.h/.cpp`) gained a bounded
  "awaiting confirmed connect" window
  (`wifiConnectConfirmationPending()`) so an unconfirmed/premature success
  signal can never wedge it again (defense in depth, not only a
  caller-side fix), plus a dedicated Wi-Fi reconnection backoff - separate
  from the unchanged per-stage DNS/Time/MQTT ladder - that grows only on
  an explicit failed/timed-out reconnect attempt and resets only on a
  full, genuinely stable success (`mqttResult(true)`), never merely by
  re-entering `WaitingForWifi` (a real, related bug: the original design
  reset backoff to the floor on every re-entry, including a momentary
  reassociation that dropped again before ever stabilizing). A genuinely
  fresh loss-of-connectivity transition still reseeds just the retry
  *deadline* from `nowMs` (both the semantically correct reaction to a new
  loss, and a fix for a 32-bit `millis()`-wraparound hazard on a
  long-untouched deadline, found and fixed during this same work before it
  was ever committed) - never the backoff *growth* itself. Both DEV
  entry points now handle `STA_CONNECTED`/`STA_GOT_IP` distinctly (only
  `GOT_IP` is a success signal), call `WiFi.setAutoReconnect(false)` once
  in `setup()`, and log each disconnect's origin
  (`origin=local_recovery`/`remote_or_unknown`) via a flag set immediately
  before `RecoverWifi`'s own `WiFi.disconnect()` call.
  `wifiDisconnectReasonName()` also now names `WIFI_REASON_ASSOC_LEAVE`/
  `HANDSHAKE_TIMEOUT`/`AUTH_EXPIRE`/`BEACON_TIMEOUT` explicitly. Heartbeat
  log spam reduced (the full credential/scan summary no longer repeats
  every 15s heartbeat while offline; RSSI added to the terse line
  instead), and `previous_reset=` now names an unrecognized reset reason
  `unknown` rather than `other`. Proven by 4 new tests in
  `test/test_dev_mqtt_state` (26 → 30) exercising exactly these two fixed
  defects plus the two related backoff/wraparound bugs found while fixing
  them; all pre-existing suites (including the other 26 in this file)
  remain unchanged and passing. The outbox is still RAM-only - this pass
  fixes *ordinary* connectivity-loss recovery so a reboot is no longer
  *required* for that case, but does not add persistence (see Future Work
  below); `event_id`/`call_id`/timestamp are still never regenerated on
  retry, and `publishPendingEvents()`'s strict-FIFO ordering (see the
  call-session entry above) is untouched. GPIO3/GPIO4, the call-session
  state machine, and `DevCommandEnvironment` are completely untouched by
  this pass. See `docs/dev-ring-simulator.md` > "Connectivity recovery
  hardening" for the full sanitized log, root-cause writeup, and the
  pending manual real-hardware retest procedure - **not yet re-validated
  on real hardware**.
- **Documentation-only follow-up: the `RING_DETECTED`/`RING_ENDED`/
  `call_id` contract is now coordinated across firmware, backend, and
  app - no longer only a firmware-side proposal.** Three still-open pull
  requests, one per repository, track it: `hjca14/interBridge#23` (this
  one), `hjca14/interBackend#27`, and `hjca14/interapp#24` - all three
  still gated on the same shared, integrated validation round before any
  of them merges. `docs/communication-protocol.md` section 16.1 and
  `docs/dev-ring-simulator.md` were updated to state this correctly
  (previously they read as "proposed, not yet backend-coordinated," which
  had gone stale). This is a documentation-only correction: no firmware
  code, test, configuration, or behavior changed. Still true and
  unchanged by this correction: `event_id` identifies one message,
  `call_id` identifies the call session and is shared between a
  `RING_DETECTED` and its `RING_ENDED`; the firmware publishes
  `RING_ENDED`, the backend processes and forwards the call's end, and
  the app ends only the session whose `call_id` matches; GPIO4/GPIO3
  remain temporary DEV-only simulators, never a production pin
  assignment; and the Si3050 and Linker Button module remain
  electrically unvalidated. At the time of this specific bullet,
  GPIO3/`RING_ENDED` and the complete call cycle were still not validated
  on real hardware - see the following bullet for the subsequent
  hardware run that closed that gap (the connectivity-recovery hardening
  itself remains not physically validated).
- **Real-hardware finding: the coordinated call-session contract
  (GPIO3/`RING_ENDED`/`call_id`) was validated end to end on a real
  ESP32-C3 Super Mini, integrated with the deployed coordinated backend
  (`hjca14/interBackend#27`) and the installed coordinated app
  (`hjca14/interapp#24`).** Confirmed, with valid local DEV credentials:
  Wi-Fi/NTP/AWS IoT MQTT/mTLS/health completed and the device appeared
  online in the app; GPIO4 started a session and published exactly one
  `RING_DETECTED`; GPIO3 ended that same session and published
  `RING_ENDED` with a distinct `event_id` and the same `call_id`; the
  event traversed firmware → AWS IoT → `telemetry_ingestion` →
  `push_sender` → FCM → app, and the app ended its call presentation on
  the correlated `RING_ENDED`; a subsequent session received a new
  `call_id`. This validates the DEV bench pipeline and the cross-repo
  contract - it does **not** validate the Si3050, Si3018/Si3019, a real
  analog intercom line, physical ringing or its end, audio, off-hook,
  physical door opening, the Linker Button module (this run used the
  same external-resistor + momentary-3V3-jumper rig as GPIO4's original
  validation for both pins), GPIO3/GPIO4 as a production pin assignment,
  production firmware/provisioning/BLE/infrastructure beyond the deployed
  DEV environment, or the connectivity-recovery hardening's actual
  loss-and-recovery behavior (this run confirmed normal connectivity, not
  a connectivity interruption - see the connectivity-recovery bullet
  above, still pending its own physical retest). The firmware still boots
  into `Idle` and the outbox remains RAM-only, unchanged by this run. See
  `docs/dev-ring-simulator.md` > "Call session addition
  (GPIO3/`RING_ENDED`/`call_id`): hardware-validated, integrated with
  backend and app" for the full record, and
  `docs/communication-protocol.md` section 16.1's "Cross-repo
  coordination and validation status" for the contract-level summary.

## Future Work

*(Prior-pass items - characterize the intercom circuit, choose an audio
codec - remain exactly as open. A Si3050 bring-up *foundation* now exists
(see Technical Debt above), but real ring detection, off-hook, line
characterization, and audio are still all open. The concrete next steps
are the Open Questions and Technical Debt items above, plus:)*

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
