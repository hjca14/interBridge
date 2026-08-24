# InterBridge Firmware — Architecture

This document describes the current architecture of the InterBridge
firmware. For project status, open decisions and change history, see
[`CONTEXT.md`](../CONTEXT.md) — this file describes *how the pieces fit
together*, CONTEXT.md tracks *what is true right now*.

## Goals

- Keep business logic (state machine, intercom logic, protocol/command
  handling, provisioning/OTA coordination) independent of Arduino/ESP32
  APIs so it can be unit tested on a host machine, ahead of final
  hardware and AWS infrastructure being available.
- Isolate every hardware-, protocol-, or cloud-specific unknown behind an
  interface, so those decisions can be made later without reworking the
  rest of the firmware.
- Keep `main.cpp` a thin composition root: it wires modules together and
  contains no business logic itself, even as the number of things it
  wires has grown substantially.

## Module map

```text
src/
├── main.cpp          Composition root: setup()/loop(), wires modules together.
│
├── core/              Cross-cutting, hardware-independent building blocks.
│   ├── events.h/.cpp        Strongly-typed internal Event/EventType.
│   ├── state_machine.h/.cpp  Call-flow state machine.
│   ├── logger.h/.cpp        Structured logging with a swappable sink.
│   ├── random_id.h/.cpp     Secure 128-bit ID generation (event_id/device_id).
│   └── version.h            FIRMWARE_VERSION.
│
├── hardware/          Hardware Abstraction Layer (HAL).
│   ├── gpio.h/.cpp           IHardwareIO (intercom line/door) + stub.
│   ├── clock.h/.cpp          IClock (monotonic + wall-clock time) + stub/fake.
│   ├── button.h/.cpp         Config/reset button: debounce + hold thresholds.
│   ├── status_indicator.h/.cpp  Semantic LED feedback (no GPIO chosen) + stub/fake.
│   └── system_control.h/.cpp ISystemControl (restart).
│
├── intercom/           Intercom business logic (electrically agnostic).
│   ├── line_detector.h/.cpp  Off-hook/on-hook edge detection over IHardwareIO.
│   ├── intercom.h/.cpp       Intercom facade; door output reports real success/failure.
│   └── si3050/         Phase 3A Si3050 DAA foundation (Rev A hardware, not
│       │                yet built/validated) - see docs/si3050-bringup.md.
│       ├── si3050_pins.h            Rev A pin map + compile-time collision checks.
│       ├── si3050_config.h/timing.h PCLK/FSYNC targets + datasheet-derived timing formulas.
│       ├── si3050_bus.h/.cpp        ISi3050Bus (SPI + CS) + Esp32/fake.
│       ├── si3050_pcm_clock.h/.cpp  IPcmClock (PCLK/FSYNC) + Esp32/fake.
│       ├── si3050_reset.h/.cpp      ISi3050Reset (/RESET) + Esp32/fake.
│       ├── si3050_delay.h/.cpp      IDelayProvider (injectable bring-up wait) + Esp32/fake.
│       ├── si3050_controller.h/.cpp Si3050Controller: electrical bring-up sequence, gates SPI.
│       └── ring_detector.h/.cpp     ISi3050RingInput (/RGDT) + debounced RingDetector.
│
├── audio/              Audio abstraction (not implemented).
│   └── audio.h/.cpp     IAudioIO interface + NullAudioIO placeholder.
│
├── storage/            Persistent key-value storage abstraction.
│   ├── persistent_store.h    IPersistentStore interface.
│   ├── memory_store.h/.cpp   In-memory implementation (native tests).
│   ├── nvs_store.h/.cpp      ESP32 NVS implementation (stub).
│   └── credential_store.h/.cpp  Isolates certificate/private-key access.
│
├── provisioning/       Device identity, BLE-first onboarding, factory reset.
│   ├── device_identity.h/.cpp        Stable device_id + setup_code load-or-generate.
│   ├── ble_provisioning.h/.cpp       BLE advertisement model + Wi-Fi credential transfer (real BLE stack stubbed - see below).
│   ├── provisioning_manager.h/.cpp   Dedicated onboarding state machine (9 states); converges every trigger.
│   ├── fleet_provisioning.h/.cpp     AWS IoT Fleet Provisioning (CSR flow) - now invoked by ProvisioningManager.
│   └── factory_reset_coordinator.h/.cpp  Clears user config, preserves identity + setup_code.
│
├── network/            Wi-Fi + MQTT transport + protocol plumbing.
│   ├── wifi.h/.cpp            IWifiConnection + WifiManager (Arduino) + fake.
│   ├── wifi_fake.cpp          FakeWifiConnection (native-safe, separate TU - see below).
│   ├── mqtt_topics.h/.cpp     Central builder for every MQTT topic used.
│   ├── mqtt_transport.h/.cpp  IDeviceTransport + real 256dpi/MQTT/WiFiClientSecure adapter + fake.
│   ├── reconnect_manager.h/.cpp  Non-blocking exponential backoff + full jitter.
│   ├── health_reporter.h/.cpp    Health/Shadow publish cadence.
│   └── protocol.h/.cpp        Legacy ICommunicationProtocol/NullProtocol - superseded, unused by main.cpp.
│
├── protocol/           Custom message model + command handling.
│   ├── messages.h/.cpp        DeviceEvent/HealthReport/CommandResponse/DeviceCommand + JSON.
│   ├── command_cache.h/.cpp   Duplicate-command protection (in-memory + persistent).
│   ├── command_handler.h/.cpp Time-safety validation + OPEN_DOOR/RESTART dispatch.
│   └── event_outbox.h/.cpp    Offline-safe event queue (in-memory + persistent).
│
├── aws/                AWS IoT Core-specific integrations.
│   ├── device_shadow.h/.cpp   Named Shadow "interbridge": reported/desired.
│   └── jobs.h/.cpp            AWS IoT Jobs client + coordinator (drives OTA).
│
└── ota/                Firmware update.
    ├── firmware_validation.h/.cpp  SHA-256 + signature verification (signature stubbed).
    └── ota_manager.h/.cpp          OtaCoordinator: version→download→verify→install→confirm.
```

`include/` is still unused (see `include/README.md`). `test/` holds 32
native unit test suites, one per PlatformIO test directory - see Testing
strategy below.

## Layering / dependency direction

```text
                    main.cpp  (composition root, Arduino-only)
                        │
                        ▼
   ┌──────────────────────────────────────────────────────────┐
   │  core/  intercom/  audio/  protocol/  provisioning/  aws/  ota/  │  business logic
   └──────────────────────────────────────────────────────────┘
                        │            depends on interfaces only
                        ▼
   ┌──────────────────────────────────────────────────────────┐
   │        hardware/ (HAL)       storage/       network/        │
   └──────────────────────────────────────────────────────────┘
                        │
                        ▼
              ESP32-C3 / Arduino APIs (MQTT/mTLS wired; other platform adapters may remain stubs)
```

`core/`, `intercom/`, `audio/`, `protocol/`, `provisioning/`, `aws/` and
`ota/` never include `Arduino.h` directly - they depend on interfaces
(`IHardwareIO`, `IClock`, `IWifiConnection`, `IDeviceTransport`,
`IPersistentStore`, `IBleProvisioning`, `IJobsClient`, `IOtaPlatform`,
etc.), never their concrete `Esp32*` implementations. This is what makes
the 24 native test suites possible without any real hardware, Wi-Fi, or
AWS account: every test injects a fake/mock implementing the relevant
interface.

`network/wifi.cpp` (the real `WifiManager`) is the one file that *does*
depend on Arduino (`WiFi.h`) directly, because Wi-Fi itself is a
confirmed requirement, not an unknown - but even it sits behind
`IWifiConnection` now, specifically so `provisioning/provisioning_manager.h`
can depend on the interface and stay natively testable via
`FakeWifiConnection` (implemented in the separate, native-safe
`network/wifi_fake.cpp`, since `wifi.cpp` itself is excluded from the
native build).

`network/protocol.h`'s `ICommunicationProtocol`/`NullProtocol` predates
the AWS IoT Core decision and is no longer constructed by `main.cpp` -
see CONTEXT.md > Decisions for why it wasn't deleted outright.

## Event flow (current)

```text
IHardwareIO.readLineState()
        │
        ▼
   LineDetector.update()  ──emits──▶  core::Event{OffHook | OnHook}
        │
        ▼
   Intercom.update()  (currently pass-through)
        │
        ▼
   main.cpp: updateIntercom()
        │  ├──▶ Logger::event(...)
        │  ├──▶ StateMachine.handleEvent(event)  ──▶  Logger::stateTransition(...)
        │  └──▶ toProtocolEvent(event.type)  ──▶  ProtocolEventName (if mapped)
        │             │
        │             ▼
        │       EventOutbox.enqueue(event_id, DeviceEvent.toJson())
        ▼
   main.cpp: updateNetwork()  (only when Wi-Fi + MQTT connected)
        │
        ▼
   for each pending outbox entry: IDeviceTransport.publish(eventsIngest topic)
        │  on success ──▶  EventOutbox.dequeue(event_id)
```

`core::EventType` (internal, drives the state machine) and
`protocol::ProtocolEventName` (the public MQTT vocabulary) are
deliberately different types - `main.cpp`'s `toProtocolEvent()` is the
single, explicit mapping point between them, and it's a partial mapping
on purpose (e.g. `DoorOpenRequested` and Wi-Fi connectivity events don't
map to anything - see CONTEXT.md > Decisions).

Incoming commands follow the mirror path:

```text
IDeviceTransport.subscribe(commands topic, handleIncomingCommand)
        │
        ▼
   parseCommand(payload)  ──▶  CommandParseResult (Ok | InvalidPayload | ...)
        │
        ▼
   CommandHandler.handle(command)
        │  ├── IDedupCache.find()/record()   (duplicate protection)
        │  ├── IClock time-safety checks      (issued_at/expires_at)
        │  └── Intercom / ISystemControl       (actual dispatch)
        ▼
   CommandResponse.toJson()  ──▶  IDeviceTransport.publish(responses topic)
```

## State machine

```text
   BOOT
     │  finishBoot()  (explicit call from main.cpp, not an event)
     ▼
   IDLE
     │  RingDetected
     ▼
  RINGING ──OnHook──▶ IDLE   (caller hung up before being answered)
     │
     │  OffHook
     ▼
  IN_CALL
     │  OnHook
     ▼
   IDLE

   (any state) ──reportFault()──▶ ERROR   (no recovery path yet)
```

Unchanged from the initial pass, and deliberately so: none of the new
provisioning/OTA/network work justified adding states like
`CONNECTING_WIFI`, `PROVISIONING`, or `UPDATING` to *this* state machine.
Provisioning and OTA have their own, separate lifecycle states
(`ProvisioningState` in `provisioning_manager.h`, `OtaResult` in
`ota_manager.h`) instead of being folded into the core call-flow state
machine - see CONTEXT.md > Decisions for why.

## Provisioning lifecycle (BLE-first onboarding)

`ProvisioningManager` is a dedicated onboarding coordinator with its own
9-state machine (`ProvisioningState`), separate from `core::StateMachine`
(the intercom call-flow machine) - see
`docs/communication-protocol.md` section 7.5 for the full rationale and
diagram. Summary:

```text
   Trigger: no Wi-Fi credentials at boot (checkAtBoot),
   or physical button held ~3s on an already-provisioned device (requestProvisioning)
                                    │
                                    ▼
                        ProvisioningAvailable
                (IBleProvisioning advertises: BleAdvertisementInfo)
                                    │
                        BLE central connects
                                    ▼
                          BleSessionActive
                       (advertising stops)
                                    │
                  Wi-Fi credentials received over BLE
                                    ▼
                            ConnectingWifi
                       (IWifiConnection.begin())
                                    │
                        Wi-Fi reports connected
                                    ▼
                    has a certificate already? ──yes──▶ CloudConnecting
                                    │no
                                    ▼
                           FleetProvisioning
              (FleetProvisioningCoordinator.provision(deviceId))
                                    │
                        success │       │ failure
                                 ▼       ▼
                       CloudConnecting   ProvisioningFailed
                                 │       (PROVISIONING_FAILED event;
                                 ▼        resumes ProvisioningAvailable
                             Provisioned  if still within the 5-minute
                (PROVISIONING_COMPLETED  window - see CONTEXT.md)
                       event queued)
```

`ENTER_PROVISIONING` is deliberately **not** one of the triggers above -
it is not remotely executable in protocol v1 (see
`docs/communication-protocol.md`). AWS IoT Fleet Provisioning is now
genuinely invoked as part of this flow (a change from the prior pass,
where `FleetProvisioningCoordinator` existed but nothing called it - see
CONTEXT.md > Change Log). `CloudConnecting` is a hand-off point, not a
verified MQTT connection: `ProvisioningManager` does not itself call
`IDeviceTransport.connect()` - `main.cpp`'s ordinary `updateNetwork()`
loop (with `ReconnectManager`) owns that, so connection/backoff logic
isn't duplicated between the two.

QR scanning and manual `setup_code` entry (`docs/communication-protocol.md`
section 7.3) are handled entirely by the app/backend to resolve *which*
physical device to open a BLE session with - by the time
`ProvisioningManager` sees anything, a BLE session already exists, so
there is exactly one code path here regardless of how the app found the
device.

## OTA lifecycle

```text
   IJobsClient.checkPendingJob()
            │
            ▼
   JobsCoordinator reports IN_PROGRESS
            │
            ▼
   OtaCoordinator.apply(version, url, sha256)
            │
      version check ──▶ VersionRejected
            │
      IOtaPlatform.downloadAndHash(url) ──▶ DownloadFailed
            │
      IFirmwareVerifier.verifySha256() ──▶ HashMismatch
            │
      IFirmwareVerifier.verifySignature() ──▶ SignatureInvalid  (always, with DefaultFirmwareVerifier - see CONTEXT.md)
            │
      IOtaPlatform.installAndReboot() ──▶ InstallFailed
            │
      IOtaPlatform.confirmBootOrRollback() ──▶ BootValidationFailed
            │
            ▼
          Success
            │
            ▼
   JobsCoordinator reports SUCCEEDED / FAILED(reason)
```

## Testing strategy

Two PlatformIO environments exist:

- `esp32-c3` — the real firmware, `framework = arduino`, with
  `lib_deps = bblanchon/ArduinoJson@^7.0.0`.
- `native` — compiles the hardware-independent subset of `src/` (42 of
  45 `.cpp` files) together with Unity tests under `test/` (32 suites),
  using the host's own C++ compiler.

`main.cpp`, `dev/mqtt_smoke_main.cpp`, and `network/wifi.cpp` are excluded
from the `native` build (via `build_src_filter` in `platformio.ini`)
because they include Arduino-only headers unconditionally. Every other
file - including
`hardware/clock.cpp`, `hardware/button.cpp`, `hardware/system_control.cpp`,
`core/random_id.cpp`, and `hardware/gpio.cpp` - compiles natively because
their Arduino-specific code is guarded with `#ifdef ARDUINO` and falls
back to an inert stub body otherwise, matching the pattern established by
`Esp32GpioHardware` in the initial pass.

See `CONTEXT.md > Tests` for what is currently covered, exactly how it
was validated without a real PlatformIO/gcc toolchain in this
environment, and what still needs on-hardware validation.

## Why these boundaries specifically

*(Carried over from the initial pass - `IHardwareIO` as a single narrow
interface, `NullAudioIO`/legacy `NullProtocol` instead of null pointers -
plus new ones from this pass:)*

- **`storage/persistent_store.h` as one generic key-value interface**
  rather than typed accessors per concern, with narrower wrappers built
  on top (`DeviceCredentialStore`) only where the isolation matters (raw
  private key access). Everything that must survive reboot - Wi-Fi
  credentials, device identity, dedup cache, event outbox - shares one
  storage abstraction and one native (`MemoryStore`) test double, instead
  of each module inventing its own persistence.
- **`IDeviceTransport` is deliberately narrow** (connect/publish/
  subscribe/poll, no topic or JSON knowledge) so the MQTT transport layer
  doesn't need to change when the protocol's message shapes change, and
  vice versa. `mqtt_topics.h` and `protocol/messages.h` depend on
  nothing from `mqtt_transport.h`.
- **Phase 2D command processing is fail-closed.** `RemoteCommandProcessor`
  strictly parses the device-scoped command payload, delegates to
  `CommandHandler`, and publishes its two logical responses through
  `IDeviceTransport` at QoS 1. Native tests use only `FakeDeviceTransport`.
  `Esp32AwsIotTransport` now uses the injected MQTT/mTLS adapter; offline tests
  demonstrate its contract, but no real AWS subscription/publication was executed in this change.
- **Door-open capability is explicit:** `Disabled` is the default and only
  operational Phase 2D mode. `Dtmf` and `Relay` are reserved names without
  sequence, key, GPIO, pulse, or physical implementation. A valid request is
  accepted for processing and then rejected as `CAPABILITY_DISABLED` without
  calling intercom, system control, provisioning, reset, or hardware.
- **No custom Last Will / availability topic.** AWS IoT Core's own
  connectivity lifecycle events are the intended authoritative
  online/offline signal (see `docs/communication-protocol.md`), so
  `IDeviceTransport` has no `setWill()`/availability API to avoid
  building a mechanism that would just be redundant with AWS's own.
- **`ProvisioningManager` takes `IWifiConnection`, not `WifiManager`**
  purely for testability - see CONTEXT.md > Decisions for the specific
  native-build linking problem this solves.
- **Command handling, provisioning, and OTA each have their own fake
  transports/clients** (`FakeDeviceTransport`, `FakeBleProvisioning`,
  `FakeJobsClient`, `FakeOtaPlatform`, `FakeFleetProvisioningTransport`)
  rather than one shared generic mock, because each protocol
  (MQTT pub/sub, BLE provisioning, AWS Jobs, HTTPS+partition OTA, Fleet
  Provisioning's CSR flow) has a genuinely different shape - forcing them
  through one interface would have made every fake's test double
  simulate operations it doesn't actually have.
- **`setup_code` lives in `DeviceIdentity`/`DeviceIdentityProvider`, not
  a separate module**, because it's generated and persisted exactly like
  `device_id` (load-or-generate-once, never regenerated) - introducing a
  parallel "setup code provider" class would have duplicated that logic
  for no benefit. It stays a distinct *field*, not folded into
  `device_id` itself, because the two have different formats, different
  audiences (machine vs. human), and different lifecycles in principle
  (see `docs/communication-protocol.md` section 4's terminology note on
  why the three onboarding-adjacent identifiers must never be conflated).
- **`BleAdvertisementInfo` is a plain data struct built by a pure
  function** (`buildBleAdvertisementInfo(deviceId)`), not a method on
  `IBleProvisioning`, so the "what do we advertise" question is testable
  in isolation from "how do we actually advertise it" (still a stub).
- **`BleSecurityMode` has no plaintext/"None" enumerator** - a structural
  choice, not just a documented rule, so a future real implementation
  cannot silently default to an insecure session merely by omitting a
  case in a switch statement.
- **`ProvisioningManager` does not verify the first MQTT connection
  itself** (`CloudConnecting` is a hand-off, not a blocking wait) to
  avoid duplicating `main.cpp`'s existing `ReconnectManager`-based
  connect/backoff loop - see the Provisioning lifecycle section above.

## DEV MQTT smoke isolation

The smoke entry point delegates connectivity decisions to the native-testable
`DevMqttSmokeState`: `WaitingForWifi` → `WaitingForDns` → `WaitingForTime` →
`WaitingForMqtt` → `Online`. Each external operation is requested only after its
prerequisites, with rollover-safe capped backoff. Wi-Fi loss invalidates the
chain so DNS, time gating, MQTT, subscription, and health publication are
re-established in order. This remains isolated from the still-stubbed production
transport architecture.

`src/dev/mqtt_smoke_main.cpp` is compiled only by `esp32-c3-dev-mqtt`; the normal
firmware excludes it. PR #6 implemented and compiled the real transport, but the
previous harness bypassed it with direct MQTT/TLS objects and a parallel handler.
The corrected composition uses `Esp32AwsIotTransport`, `RemoteCommandProcessor`,
the existing deduplication, and `CommandHandler` with capability disabled. Its
hardware and system-control dependencies are explicitly non-actuating. Ignored DEV
credentials live only in a transient `MemoryStore`; this is not reboot persistence.
Production NVS, BLE/Fleet Provisioning, and onboarding remain pending.


## MQTT/mTLS command lifecycle (Phase 2D)

The composition root retains the existing layering: Wi-Fi readiness gates MQTT;
`ReconnectManager` supplies non-blocking bounded exponential full jitter; a successful
connection resets the backoff and causes exactly one QoS 1 subscription through
`RemoteCommandProcessor`. Disconnecting clears both transport callback and composition
subscription state. `IMqttClient` is the native-test seam; the ESP32 adapter delegates
TLS to `WiFiClientSecure` and MQTT 3.1.1 to `256dpi/MQTT`, rather than implementing either.
The inbound boundary checks the exact `MqttTopics::commands()` value and 8 KiB limit
before the existing parser/handler. Outbound command results use only
`MqttTopics::responsesIngest()`, QoS 1, `retain=false`. No LWT is configured. Failure at
configuration, connect, subscribe, validation, or publish remains fail-closed.

`Esp32AwsIotTransport` tracks its own session-valid flag independent of the
underlying `IMqttClient::connected()` bookkeeping: any publish/subscribe/poll/connect
failure marks the session untrusted, and `connect()` always tears the previous
session down (`client_->disconnect()`) before configuring TLS again, rather than
assuming the caller already did so or that the last session is safe to reuse. The
ESP32 adapter additionally allocates a fresh `WiFiClientSecure` per connection
attempt instead of reusing the same socket/TLS object across a broken session -
reusing it is what produced the `setSocketOption(): ... Bad file number` pattern
observed after a publish failure on real hardware. `IMqttClient::lastErrorCode()`
exposes the underlying client's own numeric failure code (256dpi/MQTT's
`lwmqtt_err_t` on ESP32, always `0` for the native-only stub); `publish()` logs it
alongside the existing failure message - a bare sanitized number, never
topic/payload/identifier content.

`RemoteCommandProcessor` holds a small bounded (`kMaxOutboxSize = 2`) in-RAM outbox
for pending ACCEPTED/terminal responses. Every call site attempts at most one
response publish - full stop. A brand-new command (`processPayload()`) attempts to
publish ACCEPTED once; the terminal is always deferred to a later
`processPending()` call, even when ACCEPTED itself just published successfully -
it is never attempted a second time in the same call. Two back-to-back QoS 1
publishes with no `transport.poll()` in between (which happens naturally between
separate `processPending()` calls in the real main loop) is what produced
intermittent "terminal response publish failed" even when ACCEPTED itself
succeeded under a few consecutive commands on real hardware. `processPending()`
never starts a new queued command while the outbox is non-empty, so under that
invariant at most one command's own ACCEPTED+terminal pair is ever pending -
`kMaxOutboxSize` is an explicit, logged backstop for that invariant being bypassed
(e.g. a direct `processPayload()` call), not working capacity for several
commands' worth of responses. When the outbox has an item, `processPending()`
attempts to publish only the item at the front, at most once, then returns; on
success the item is popped, on failure it stays queued and the normal
invalidate/reconnect/backoff flow handles the retry on a later call. Reaching
capacity never evicts an already-pending entry (that could silently drop a
promised ACCEPTED while keeping only its terminal); the new response is rejected
and logged instead. Draining only republishes already-serialized bytes, including
the response's own sanitized terminal code, so a retried terminal still reports
its real status - it never re-invokes `CommandHandler` or the dedup cache. The
outbox is RAM-only and does not survive reboot, matching the event outbox's
documented in-memory-during-development posture (section 17 of the protocol doc).

`CommandDiagnosticStage` distinguishes exactly why a terminal is still pending -
`TerminalDeferred` (ACCEPTED just published; not attempted yet, not a failure),
`TerminalQueuedBehindAccepted` (ACCEPTED itself failed; not attempted yet, not a
failure - `AcceptedPending` alongside it already reports the real failure), and
`TerminalPublishFailed` (a real publish attempt happened and failed). None of
the first two are ever worded as a publish failure; only the third is, matching
what actually happened.

`CommandHandler::handle()` computes ACCEPTED and the terminal result synchronously,
before ACCEPTED is even attempted - safe only because `DoorOpenCapability` stays
`Disabled` and no physical action is taken. A future capability that actually
actuates hardware cannot reuse this shape unmodified: it needs to separate
validation, publishing (and confirming) ACCEPTED, and only then triggering the
physical action as a distinct step gated on that confirmation. This PR does not
implement or guarantee that ordering.
