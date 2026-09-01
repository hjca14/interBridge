# DEV physical ring simulator (Phase 3B.8)

## Objective

Phase 3B.8 adds a bench-only DEV firmware environment,
`esp32-c3-dev-ring-simulator`, that lets a physical momentary button
simulate an intercom ring and publish a real `RING_DETECTED` `DeviceEvent`
through the exact same AWS IoT Basic Ingest event pipeline production
firmware uses (topics, JSON serialization, and the outbox). It exists so
the notification pipeline downstream of the firmware (backend → push
notification → mobile call UI) can be exercised end-to-end on a bench
without a real Si3050/intercom line attached.

**This is exclusively a bench/DEV convenience.** It does not simulate
audio, off-hook, in-call, or any other intercom state - only the ring
signal's start and end. See Scope and safety below.

**A later pass (see "Call session state machine" below) extends this
environment to simulate a complete call session, not just its start**: a
second momentary input (GPIO3) simulates the same call's end and
publishes `RING_ENDED`, correlated with the `RING_DETECTED` from GPIO4 by
a shared `call_id`. **This extension is native-tested only - it has not
yet been exercised on real hardware.** The GPIO4-only 3B.8 hardware
validation recorded below still stands as-is and is not affected by it.

**Validated state (see the consolidated record below):** 3B.8's original
GPIO4-only scope is implemented, compiled, unit-tested, and validated end
to end on a real ESP32-C3 Super Mini. The successful test used a
controlled electrical stimulus rather than the Linker Button: GPIO4 was
held LOW through an approximately 10 kΩ resistor to GND, then connected
momentarily to 3V3. Wi-Fi, NTP, and AWS IoT MQTT/mTLS completed; the
health report made the device appear online in the app; and the pulse
produced exactly one event and one confirmed publish before traversing
the backend, FCM, and Android notification path.

## Scope and safety

- A **separate PlatformIO environment** (`esp32-c3-dev-ring-simulator`),
  gated behind `INTERBRIDGE_DEV_RING_SIMULATOR` - it is never linked into
  `esp32-c3` (production), `esp32-c3-dev-mqtt`, or either Si3050 clock
  probe environment, and none of those are linked into it. `main.cpp` and
  `dev/mqtt_smoke_main.cpp` are excluded from its `build_src_filter`, and
  it is excluded from theirs (see `platformio.ini`).
- **Never touches** the real Si3050 driver stack
  (`intercom/si3050/si3050_controller.*`, `ring_detector.*`,
  `si3050_pcm_clock.*`, the PCNT/generator clock probes), production
  provisioning/BLE, Wi-Fi credential handling, or the production AWS IoT
  composition root. It reuses the DEV MQTT smoke environment's
  connectivity bring-up class, `DevMqttSmokeState`
  (`src/dev/mqtt_smoke_state.*`), rather than duplicating that state
  machine - see Bench test history below for a real defect found in that
  shared class during this work.
- Publishes **only** through the existing production contract: the topic
  returned by `MqttTopics::eventsIngest()`, `Esp32AwsIotTransport` (AWS IoT
  Basic Ingest, mTLS), and the existing `IEventOutbox`
  (`MemoryEventOutbox`) - see `src/dev/dev_ring_event.*`. No second MQTT
  client, no parallel/ad hoc topic. The same applies to the health/presence
  publish added in this pass - see "Online status" below.
- GPIO4 only ever produces `RING_DETECTED`, and GPIO3 only ever produces
  `RING_ENDED` for the session GPIO4 started - see "Call session state
  machine" below. Neither GPIO, nor the pair together, simulate
  `OFF_HOOK`, `CALL_STARTED`, `CALL_ENDED`, audio, or any other
  intercom/call-state event; `RING_DETECTED`/`RING_ENDED` describe only
  the simulated ring signal's start and end, never a real answered call.
- Same DEV-only credential model as `esp32-c3-dev-mqtt`: certificates load
  into transient `MemoryStore` (not NVS), never logged.

## Wiring diagram

The diagram below records the **intended** Linker Button wiring according
to the module documentation. It was not the circuit used for the successful
3B.8 validation, and the module's real electrical behavior and wiring have
not been validated on this bench. The documentation describes `SIG` as LOW
when released and HIGH when pressed.

```
                 ESP32-C3 Super Mini              Linker Button module
                +---------------------+          +-------------------+
                |               3V3 o-+----------+ VCC               |
                |               GND o-+----------+ GND               |
                |    GPIO4 (DRX) o----+----------+ SIG               |
                +---------------------+          +-------------------+
```

- `VCC`/`V` → **3V3** (never 5V - the ESP32-C3's GPIOs are not
  5V-tolerant).
- `GND`/`G` → **GND**.
- `SIG`/`S` → **GPIO4**.
- `pinMode(GPIO4, INPUT)` - **no internal pull-up**: the module drives
  `SIG` itself in both states. The pin reads **LOW when released** and
  **HIGH when pressed** (active-high).

### Validated electrical rig: resistor + pulse (GPIO4 and now GPIO3)

The diagram above is the *intended* Linker Button wiring; the circuit
that actually passed 3B.8's hardware validation (see "Bench test
history" below) was simpler and is the one both GPIO4 and the new GPIO3
input use going forward:

```
                 ESP32-C3 Super Mini
                +-----------------------+
                |                       |
   3V3 o--------o  momentary pulse      |
                |        |              |
                |     GPIO4 (start) o---+
                |     GPIO3 (end)   o---+
                |        |              |
   GND o--------o------- +--[~10 kOhm]--+  (external resistor to GND,
                |                       |   one per GPIO, always present)
                +-----------------------+
```

- Each of GPIO4 and GPIO3 is held **LOW at rest** by its own external
  ~10 kΩ resistor to GND.
- A momentary, manual connection to **3V3** pulses that pin HIGH; release
  lets the resistor pull it back LOW.
- **Never connect GND and 3V3 directly** - the resistor is what makes a
  momentary 3V3 touch safe; without it, a stray simultaneous GND+3V3
  connection would short the rail.
- `pinMode(pin, INPUT)` for both - **no internal pull-up or pull-down**:
  the external resistor already defines the resting level, and enabling
  an internal pull here would fight it rather than complement it (the
  task instruction this environment follows is explicit that no internal
  pull may be enabled unless it is proven compatible with the external
  resistor - it is not needed here at all).
- Both pins are active-high: LOW at rest, HIGH only during the momentary
  pulse.

### Why GPIO4 and GPIO3, and why both are provisional

The validated bench board (generic 4 MB ESP32-C3 Super Mini, see
`platformio.ini`/`CONTEXT.md`) exposes only 15 GPIOs total: 0-10 and
18-21.

| Pins | Committed to |
|---|---|
| 0, 1, 2, **3**, **4**, 5, 6, 7, 8, 10 | Real Si3050 wiring (`src/intercom/si3050/si3050_pins.h`) - GPIO4 is the Si3050's DRX line (`kSi3050PinPcmDrx`); GPIO3 is the Si3050's DTX line (`kSi3050PinPcmDtx`) |
| 2, 8, 9 | BOOT/strapping pins - excluded |
| 18, 19 | Native USB D-/D+ (serial console) - reserved |
| 20, 21 | *Documentation-reserved only* for a future physical config/reset button and status LED (`kSi3050ReservedPinButton`/`kSi3050ReservedPinStatusLed`), neither implemented in any current code path. GPIO20 was tried and abandoned for this bench rig - see Bench test history. |

With no pin on this board free of *some* real or planned use, GPIO4's
choice was a **deliberate, explicit overlap with one real Si3050 pin**
(`kSi3050PinPcmDrx`, DRX). The call-session pass adds a second input,
GPIO3, for exactly the same reason and under exactly the same
constraints - it overlaps `kSi3050PinPcmDtx` (DTX), the only other choice
left once BOOT/strapping, USB, GPIO4 itself, and the GPIO20/21
Wi-Fi-drop-correlated pins (see Bench test history) are all excluded.
Both overlaps are safe **only** because `esp32-c3-dev-ring-simulator`
never compiles or initializes any Si3050/RingDetector/PCM-clock code, and
no Si3050 is physically attached to the board while this bench
environment runs. `src/dev/dev_ring_simulator_config.h`
compile-time-asserts each button pin is exactly its one approved overlap
- never any other Si3050 pin, never the BOOT/USB pins, and never the same
pin as each other - so a future change can never silently drift to an
unreviewed pin. **Neither is a production pin assignment.** Both must be
revisited once the Si3050 and the final board (with the real config/reset
button) are integrated together. The real Si3050 pin map
(`src/intercom/si3050/si3050_pins.h`) and production firmware are
completely untouched by this choice.

## Build

```
pio run -e esp32-c3-dev-ring-simulator
```

Requires the same ignored, locally generated `include/interbridge_dev_secrets.h`
as `esp32-c3-dev-mqtt` (see `docs/mqtt-dev-smoke-test.md` > Local
credentials and build for how to generate it via
`scripts/generate_dev_secrets_header.ps1`). No new secrets/macros were
added - this environment reuses `INTERBRIDGE_DEV_WIFI_SSID`,
`INTERBRIDGE_DEV_WIFI_PASSWORD`, `INTERBRIDGE_DEV_AWS_ENDPOINT`,
`INTERBRIDGE_DEV_DEVICE_ID`, `INTERBRIDGE_DEV_ROOT_CA_PEM`,
`INTERBRIDGE_DEV_CERTIFICATE_PEM`, and `INTERBRIDGE_DEV_PRIVATE_KEY_PEM`
exactly as `esp32-c3-dev-mqtt` does. CI compiles this environment against
the committed placeholder example header only (no real Wi-Fi/AWS
connection attempted, no hardware flashed) - see `.github/workflows/ci.yml`.

## Event and topic

- **Topic**: `MqttTopics::eventsIngest()` - the same AWS IoT Basic Ingest
  topic (`$aws/rules/interbridge_dev_ingest_rule/interbridge/{device_id}/events`
  for this DEV rule name) production `RING_DETECTED` events use. `RING_ENDED`
  uses the exact same topic - there is no separate topic per event type.
- **Payload**: the standard `DeviceEvent` v1 contract
  (`src/protocol/messages.h`), extended with an optional `call_id` (see
  `docs/communication-protocol.md` section 16.1 for the full contract):

  ```json
  {"protocol_version":1,"device_id":"ib-...","event":"RING_DETECTED","event_id":"evt-...","call_id":"call-...","timestamp":"2026-08-29T12:00:00Z"}
  {"protocol_version":1,"device_id":"ib-...","event":"RING_ENDED","event_id":"evt-...(different)","call_id":"call-...(same)","timestamp":"2026-08-29T12:00:45Z"}
  ```

  `timestamp` is only present once NTP has completed (same convention as
  every other `DeviceEvent` in this codebase - see `messages.h`); it is
  omitted entirely if the clock is not yet trustworthy at the moment of
  the pulse. `call_id` is a retrocompatible addition: it is omitted
  entirely from every event that isn't part of a call session (every
  event type this codebase already emitted before this pass), so their
  JSON is byte-for-byte unchanged.
- **QoS**: `AtLeastOnce` (QoS 1), matching `main.cpp`'s production event
  publishing, for both event types.

## Call session state machine

`DevRingEventCoordinator` (`src/dev/dev_ring_event.*`) owns a minimal,
explicit two-state machine for one simulated call session at a time:

```text
              GPIO4 valid pulse                GPIO3 valid pulse
              (new call_id,                    (RING_ENDED, same
               RING_DETECTED)                   call_id)
        ┌───────────────────────▶ RINGING ───────────────────────┐
        │                            │                            │
      IDLE ◀─────────────────────────┘                            ▼
        ▲                    kDevCallTimeoutMs elapsed        (RING_ENDED
        │                    with no GPIO3 pulse               enqueued)
        └──────────────────────────────────────────────────────── IDLE
```

Rules this state machine enforces (each has a corresponding native test
in `test/test_dev_ring_event`):

- **`IDLE`**: no simulated call is active. A GPIO3 pulse here is ignored
  (`EndIgnoredNoActiveCall`) - it can never produce a `RING_ENDED` with no
  session to correlate it to.
- **A valid GPIO4 pulse in `IDLE`** generates a brand-new `call_id`,
  enqueues `RING_DETECTED` (own `event_id`, this `call_id`), and enters
  `RINGING`. The session is only ever considered active once this enqueue
  has happened - `MemoryEventOutbox::enqueue()` is a bounded in-RAM append
  that cannot itself fail as a fallible I/O call would, so "enqueued" and
  "session active" are inseparable here.
- **A valid GPIO4 pulse while already `RINGING`** is ignored
  (`StartIgnoredAlreadyRinging`): no new `call_id`, no second event, the
  active session is untouched.
- **A valid GPIO3 pulse in `RINGING`** enqueues `RING_ENDED` reusing the
  exact active `call_id` (a new, different `event_id`) and returns to
  `IDLE`.
- **Debounce/lockout** is the same `DevRingButtonController` both GPIO4
  and GPIO3 already share (50 ms debounce, 250 ms post-event lockout) -
  contact bounce or a held-HIGH input can never produce more than one
  qualifying edge per physical pulse, on either pin.
- **Safety timeout** (`kDevCallTimeoutMs`, default 60000 ms, aligned with
  the app's own ring-timeout fallback): if `RINGING` persists this long
  with no qualifying GPIO3 pulse, the coordinator enqueues `RING_ENDED`
  for the active session itself and returns to `IDLE` - a simulated call
  can never remain `RINGING` forever. This reason (button vs. timeout) is
  **local-log-only**: `docs/communication-protocol.md` does not yet
  define a coordinated wire field for it, and this DEV simulator does not
  invent one unilaterally - see `dev_ring_simulator_main.cpp`'s log lines
  for exactly what the timeout path emits instead.
- **GPIO3 and the timeout racing in the same loop tick still produce only
  one `RING_ENDED`**: `DevRingEventCoordinator::update()` resolves a
  qualifying button edge before ever evaluating the timeout condition, and
  returns immediately once it does - the timeout branch is structurally
  unreachable in the same call that already handled a button edge.
- **A later call, after a clean end, always gets a new `call_id`** - the
  same random source that generates `event_id`s generates `call_id`s, so
  two sessions from the same boot never collide.
- **A firmware restart always begins `IDLE`**: this composition is
  reconstructed from scratch at boot (RAM-only, like the rest of this DEV
  environment's state - see "Offline behavior and replay" below), and
  there is no mechanism, and deliberately none is added, that tries to
  restore a previously-active simulated call as still active after a
  reset.
- **Publish ordering**: `publishPendingEvents()` (`src/dev/dev_ring_event.cpp`)
  stops at the first failed publish instead of continuing to later,
  newer-enqueued entries - so a `RING_ENDED` already queued behind its own
  `RING_DETECTED` can never be published ahead of it just because a retry
  attempt for the `RING_DETECTED` happens to fail on a given loop
  iteration. This is a real, previously-latent ordering gap this pass
  fixes (see `test_retry_preserves_ids_and_ring_detected_publishes_before_ring_ended`
  in `test/test_dev_ring_event`), not a change in the outbox's shape.

None of this is a general-purpose call-flow simulator: it does not model
off-hook, in-call, hold, or any Si3050/audio state, and it is entirely
separate from `core::StateMachine` (the real firmware's own BOOT/IDLE/
RINGING/IN_CALL/ERROR machine), which this environment never touches.

## Offline behavior and replay

A pulse on either GPIO always enqueues into `MemoryEventOutbox` first,
regardless of connectivity - the button-to-outbox path
(`DevRingEventCoordinator::update()`, `src/dev/dev_ring_event.*`) never
touches the transport directly. Publishing is a separate step
(`publishPendingEvents()`) that only runs once `transport.isConnected()`
is true, and only dequeues an entry after a **successful** publish - an
offline or failed attempt leaves that entry, and everything queued behind
it, untouched (see "Call session state machine" > Publish ordering
above), with the same `event_id`/`call_id`, for the next attempt. This
exactly mirrors `main.cpp`'s production `updateNetwork()` outbox loop;
see `src/dev/dev_ring_event.cpp`.

The outbox is **RAM-only** (`MemoryEventOutbox`, not
`PersistentEventOutbox`) - a reboot while an event is still queued loses
it, the same DEV-only tradeoff `mqtt_smoke_main.cpp` already accepts for
its credentials. This is acceptable for a bench convenience tool;
production's own outbox path is unaffected (this environment does not
compile `main.cpp` at all).

## Online status: local connectivity vs. app presence

A real bench test reached `[DEV RING] state mqtt -> online` (Wi-Fi → DNS
→ NTP → MQTT all connected) while the companion app still showed the
device offline. **`state ... -> online` reported by this simulator (and
by `esp32-c3-dev-mqtt`) means local connectivity only** - the firmware's
own `DevMqttSmokeState` reached its terminal `Online` state. It says
nothing by itself about whether the backend/app currently consider the
device "present."

`esp32-c3-dev-mqtt` already publishes a periodic `HealthReport` (device
ID, firmware version, `intercom_state`, uptime, Wi-Fi RSSI, free heap) to
`MqttTopics::healthIngest()` (AWS IoT Basic Ingest, QoS `AtMostOnce`),
gated on Wi-Fi/time/MQTT all being valid and a cadence
(`kDevHealthIntervalMs`, 60s) - see `publishHealth()` in
`src/dev/mqtt_smoke_main.cpp`. This pass adds the **identical** publish
to `esp32-c3-dev-ring-simulator` (same fields, same topic, same QoS, same
cadence constant). In the successful end-to-end run, the health report was
published and the device then appeared online in the app, confirming this
presence path against the deployed DEV backend/app. The health publish is
entirely independent of the `RING_DETECTED` event outbox - a skipped or
failed health publish never touches `eventOutbox`, and vice versa.

## Dependency on Phases 3B.6/3B.7

This phase only makes the **firmware** publish a real `RING_DETECTED`
event (and, as of this pass, a health/presence signal). Turning that into
a phone notification requires the rest of the pipeline, tracked
separately (see `docs/roadmap-3b.md`):

- **Phase 3B.6** (backend FCM sender): `telemetry_ingestion` invokes
  `push_sender`, which delivers the push notification via Firebase Cloud
  Messaging. **Implemented, deployed, and exercised in DEV** by the hardware run:
  the GPIO-triggered event traversed `telemetry_ingestion` and `push_sender`
  to FCM and the Android app.
- **Phase 3B.7** (notification preference application): the backend
  applies the user's/app's notification preferences before deciding
  whether/how to notify. **Implemented and deployed in DEV.**

## Command processing (Phase 3B.8 cumulative pass)

The end-to-end 3B.8 hardware run (see Bench test history below) proved
`RING_DETECTED`/health, but this environment did not yet subscribe to the
commands topic at all - a real `OPEN_DOOR` sent from the app while this
firmware was loaded was never received, let alone processed, because
nothing on the device was listening. This gap existed only because
`esp32-c3-dev-ring-simulator` had never adopted `esp32-c3-dev-mqtt`'s own
already-real-hardware-validated command-processing composition (see
`docs/mqtt-dev-smoke-test.md`).

This pass adds it, reusing the exact same classes `esp32-c3-dev-mqtt`
already uses - `RemoteCommandProcessor`, `CommandHandler`,
`InMemoryDedupCache`, and the non-actuating `DisabledHardware`/
`DisabledSystemControl` pair. A first version of this fix shared only the
`DisabledHardware`/`DisabledSystemControl` stand-ins and the diagnostic log
wording via two small headers, while the composition and command-processing
cycle around them (`InMemoryDedupCache`, `Intercom`, `CommandHandler`,
`RemoteCommandProcessor`, the commands-topic subscription, and draining
pending responses) were still hand-copied into each `*_main.cpp` - the same
kind of manual duplication that caused this gap in the first place, just
narrower. That is now fixed too: `src/dev/dev_command_environment.h/.cpp`
defines `DevCommandEnvironment`, one class that owns the entire composition
behind a small `subscribe()`/`processPending()`/`setDiagnosticCallback()`
surface, and both `esp32-c3-dev-mqtt` and `esp32-c3-dev-ring-simulator`
construct exactly one instance of it and call only that surface - neither
entry point declares `RemoteCommandProcessor`, `CommandHandler`, or
`InMemoryDedupCache` itself anymore. The per-stage diagnostic log wording is
still shared separately, from `src/dev/dev_command_diagnostics.h/.cpp`
(Arduino/Serial-only, so it stays outside the natively-testable
`DevCommandEnvironment` class itself; each entry point wires its own log
prefix through `setDiagnosticCallback()`).
`scripts/check_repo_safety.py` greps both entry points for the
`DevCommandEnvironment commandEnv`/`commandEnv.subscribe()`/
`commandEnv.processPending()` fragments, so a future omission of either call
in either environment fails CI, not just review. `esp32-c3-dev-ring-simulator`
now:

- subscribes to `interbridge/{device_id}/commands` (QoS 1) on every
  successful MQTT connect, exactly like `esp32-c3-dev-mqtt` - a failed
  subscription now tears the connection down and retries, the same
  `subscribed` bookkeeping `esp32-c3-dev-mqtt` already uses, so a dropped
  subscription is never silently left unresolved after a reconnect;
- drains and processes commands every loop iteration via
  `commandEnv.processPending()`, on the same one-response-publish-per-call
  discipline documented in `docs/mqtt-dev-smoke-test.md`;
- still only reaches `DoorOpenCapability::Disabled` (`CommandHandler`'s own
  compile-time constant, shared, not overridden here) - a valid `OPEN_DOOR`
  still only ever produces `ACCEPTED` then `REJECTED/CAPABILITY_DISABLED`.
  No door, relay, DTMF, GPIO, or system action is genuinely performed.

**What this closes, and what it does not**: `RING_DETECTED` *generation and
outbox* (`DevRingEventCoordinator`, `MemoryEventOutbox`,
`publishPendingEvents`) are unchanged by this pass - the button-press path,
debounce, offline queueing, and `event_id` stability are exactly what the
3B.8 hardware run validated. It is **not** true that the surrounding
session/connectivity behavior is byte-for-byte unchanged, though: `Online`
is now only reached after a successful command-topic subscription (not just
Wi-Fi/DNS/NTP/MQTT connect), a failed subscription now tears the connection
down and retries, and every loop iteration now also drains pending command
responses (`commandEnv.processPending()`) alongside the outbox drain. So
`RING_DETECTED` publishing itself did not change, but *when* the session is
considered ready, and what else happens on it after connecting, did.

What is proven automatically: all 40 native Unity suites pass - the 39 that
existed before this pass (unchanged logic - `CommandHandler`,
`RemoteCommandProcessor`, dedup, `DevRingEventCoordinator`,
`DevRingButtonController`, `HealthReporter`, `DevMqttSmokeState`, and every
other existing suite), plus a new `test_dev_command_environment` suite that
exercises the shared `DevCommandEnvironment` composition directly: it
subscribes to the exact commands topic at QoS 1, drives a full valid
`OPEN_DOOR` through to `ACCEPTED` then `REJECTED/CAPABILITY_DISABLED`
(never `COMPLETED`), and proves re-subscription after a simulated
disconnect/reconnect still processes a command correctly. Unlike the
pre-existing per-class suites (which only ever exercised `CommandHandler`/
`RemoteCommandProcessor` in isolation and could not see that
`esp32-c3-dev-ring-simulator` never constructed them at all), this suite
targets the one class both DEV entry points actually build against, so an
entry point silently omitting the composition is no longer just an
uncaught textual gap - `scripts/check_repo_safety.py` also now greps both
`*_main.cpp` files for the `DevCommandEnvironment`/`subscribe()`/
`processPending()` calls (see "Command processing" above). All three
affected PlatformIO environments (`esp32-c3`, `esp32-c3-dev-mqtt`,
`esp32-c3-dev-ring-simulator`) compile cleanly with this composition. What
is **not** proven automatically, and still needs a real ESP32 retest: that
`esp32-c3-dev-ring-simulator` actually subscribes successfully on real
AWS IoT Core, actually receives a real `OPEN_DOOR` command, and actually
publishes `ACCEPTED` then `REJECTED/CAPABILITY_DISABLED` back - this exact
combination (ring simulator + commands) has never run on real hardware.
`RING_DETECTED`/health continuing to work correctly alongside it also has
not been re-confirmed on real hardware since this pass.

## Call session addition (this pass)

This pass evolves the environment from "GPIO4 starts a simulated ring"
to "GPIO4 starts, GPIO3 ends, one simulated call session" - see "Call
session state machine" above for the state machine itself and
`docs/communication-protocol.md` section 16.1 for the wire contract.

**What changed:**

- `src/protocol/messages.h/.cpp`: added `ProtocolEventName::RingEnded`
  (`"RING_ENDED"`) and an optional `DeviceEvent::callId` (serialized as
  `call_id` only when non-empty) - retrocompatible, every pre-existing
  event type's JSON is unchanged.
- `src/dev/dev_ring_simulator_config.h`: added `kDevRingEndButtonPin`
  (GPIO3), with the same compile-time-asserted, single-approved-overlap
  pattern `kDevRingButtonPin` (GPIO4) already used.
- `src/dev/dev_ring_event.h/.cpp`: `DevRingEventCoordinator` now takes
  both buttons, owns the `Idle`/`Ringing` state machine, `call_id`
  generation, and the safety timeout; `publishPendingEvents()` now stops
  at the first failed publish instead of continuing past it (a real,
  previously-latent ordering gap this pass fixes - see "Call session
  state machine" > Publish ordering above).
- `src/dev/dev_ring_simulator_main.cpp`: wires a second
  `Esp32DevRingButtonInput`/`DevRingButtonController` for GPIO3 (now
  pin-parameterized rather than hardcoded to GPIO4), initializes GPIO3 in
  `setup()`, and logs each `DevCallSessionEventKind` distinctly in
  `loop()`. Every previously-existing capability in this file - Wi-Fi/NTP/
  MQTT bring-up, health reporting, `DevCommandEnvironment` command
  processing - is untouched.
- `docs/communication-protocol.md`: documents `RING_ENDED`/`call_id` as a
  proposed, retrocompatible extension (section 16.1), explicitly not yet
  backend-coordinated.

**What did not change:** GPIO4's own electrical behavior, debounce
constants, and `RING_DETECTED` payload shape (aside from the new optional
`call_id` field); `DevCommandEnvironment`/command processing (untouched,
no duplicated composition); the production `esp32-c3` environment (this
entire addition lives behind `INTERBRIDGE_DEV_RING_SIMULATOR`, in files
`esp32-c3` never compiles - see `platformio.ini`); `scripts/check_repo_safety.py`
(no new safety-relevant text pattern was introduced by this pass).

**What is proven automatically:** all pre-existing native suites remain
unchanged and passing, plus `test/test_dev_ring_event` (rewritten for the
two-button/call-session coordinator, 16 tests - see "Call session state
machine" above for what each behavior guarantees). **What is not proven
automatically, and needs a real ESP32 retest:** everything under "Call
session addition (GPIO3/`RING_ENDED`/`call_id`): not yet hardware-validated"
below.

## Manual flash and test procedure

The end-to-end test is complete and does not require a Linker Button
retest. The following remains an **optional, separate module-evaluation
procedure**; completing it would characterize the module, not reopen or
re-close 3B.8. The wiring is intended according to module documentation,
not physically validated here:

1. Wire the Linker Button module as shown above: `VCC`→3V3, `GND`→GND,
   `SIG`→GPIO4. Do not power it from 5V.
2. Generate `include/interbridge_dev_secrets.h` for a DEV device identity
   (a dedicated `device_id`, distinct from the DEV MQTT smoke harness's,
   is recommended so the two bench firmwares don't collide on the same
   AWS IoT Thing).
3. `pio run -e esp32-c3-dev-ring-simulator -t upload` and attach the
   serial monitor.
4. Confirm the boot log lines `[DEV RING] previous_reset=...`,
   `[DEV RING] start button initialized gpio=4 mode=INPUT active=high
   rig=external_pulldown_10k role=call_start` (this log line's wording
   changed with the call-session pass below - it no longer names the
   Linker Button module, since the code path is now rig-agnostic; the
   electrical reading it describes is unchanged), and `[DEV RING]
   config=valid ssid_bytes=N password_bytes=N placeholder=false` (a
   `config=invalid` or `placeholder=true` here means the compiled binary
   does not hold the intended credential - stop and regenerate the
   secrets header before continuing).
5. Confirm `[DEV RING] wifi scan networks_found=N
   configured_ssid_found=true|false ...` right before the first
   `WiFi.begin()` - this scan runs exactly once per boot; a reboot is
   required to refresh it.
6. Wait for `[DEV RING] state ... -> online` (Wi-Fi → DNS → NTP → MQTT →
   command subscription - `Online` is now reached only after
   `[DEV RING] command QoS1 subscription: ok`, same as `esp32-c3-dev-mqtt`;
   a failed subscription logs `: failed`, tears the connection down, and
   retries). If association fails, capture the `[DEV RING] wifi
   event=disconnected reason=N (...)` line(s) verbatim alongside the
   `config=`/`wifi scan` lines from steps 4-5.
7. Press the button once (do not hold). Expect, in order:
   - `[DEV RING] valid start detected on GPIO4; RING_DETECTED enqueued call_id=...`
   - `[DEV RING] publish confirmed count=1 remaining=0`
8. Confirm on the backend/AWS IoT side that exactly one `RING_DETECTED`
   event arrived for that `device_id`, with a fresh `event_id` and a
   populated ISO-8601 `timestamp`.
9. Press and hold: confirm no repeat event while held. Release, then
   press again: confirm a second `RING_DETECTED` with a **different**
   `event_id`.
10. Disconnect Wi-Fi (or block the AWS IoT endpoint) while leaving the
    board powered, press the button, and confirm
    `[DEV RING] offline; event queued, awaiting reconnection` - then
    restore connectivity and confirm the queued event is published on
    reconnect with the **same** `event_id` it was enqueued with.
11. Confirm `[DEV RING] health publish: ok` lines appear roughly every
    60s once online, and check whether the app now reflects the device
    as present - see "Online status" above for what this does and does
    not confirm.
12. Send a protocol-v1 `OPEN_DOOR` command on that exact device's commands
    topic (same procedure as `docs/mqtt-dev-smoke-test.md` step 5). Expect,
    in order: `[DEV RING] command received`, `[DEV RING] time validation ok
    seq=... age_s=... remaining_s=...`, `[DEV RING] ACCEPTED published
    seq=...`, then (deferred to the next loop iteration)
    `[DEV RING] terminal published seq=... code=CAPABILITY_DISABLED`.
    Verify no `COMPLETED`, `DOOR_OPENED`, DTMF, GPIO, relay, pulse, key, or
    restart action - only the protocol-v1 `ACCEPTED` then
    `REJECTED/CAPABILITY_DISABLED` responses on the responses topic.
13. Send the exact same `command_id` again: confirm the terminal response
    is replayed from the dedup cache (`CommandHandler`'s existing
    duplicate-protection path), not reprocessed.
14. Confirm a `RING_DETECTED` press (step 7) and an `OPEN_DOOR` command
    (step 12) both work correctly within the same boot, in either order -
    the two paths (event outbox vs. command processor) must not interfere
    with each other.

## Call session manual test procedure (GPIO3 + GPIO4, pending real-hardware run)

This procedure exercises the call-session addition (GPIO3/`RING_ENDED`/
`call_id`/timeout) described above. **Unlike the GPIO4-only procedure
above, this has not yet been run on real hardware** - only native-tested
(`test/test_dev_ring_event`, 16 assertions). Use the same electrical rig
for both pins - see "Validated electrical rig: resistor + pulse" above,
never the Linker Button module for GPIO3.

1. Wire two independent momentary-pulse inputs, one per pin: GPIO4 (start)
   and GPIO3 (end), each held LOW at rest by its own external ~10 kΩ
   resistor to GND, each pulsed momentarily to 3V3 to trigger. Never
   connect GND and 3V3 directly on either pin.
2. Flash and boot as in steps 2-6 above. Additionally confirm the boot
   log line `[DEV RING] end button initialized gpio=3 mode=INPUT
   active=high rig=external_pulldown_10k role=call_end`.
3. Pulse GPIO3 once *before ever pulsing GPIO4* (no active call). Expect
   `[DEV RING] GPIO3 press ignored; no active call` and confirm no event
   is enqueued or published.
4. Pulse GPIO4 once. Expect, in order: `[DEV RING] valid start detected
   on GPIO4; RING_DETECTED enqueued call_id=<A>` then `[DEV RING] publish
   confirmed count=1 remaining=0`. Record `call_id=<A>`.
5. Pulse GPIO4 again while still in this same call (do not pulse GPIO3
   first). Expect `[DEV RING] GPIO4 press ignored; call_id=<A> already
   active` and confirm no second event is enqueued - the outbox/backend
   must still show only the one `RING_DETECTED` from step 4.
6. Pulse GPIO3 once. Expect, in order: `[DEV RING] valid end detected on
   GPIO3; RING_ENDED enqueued call_id=<A>` (the **same** `<A>` from step
   4) then `[DEV RING] publish confirmed count=1 remaining=0`. Confirm on
   the backend/AWS IoT side that this `RING_ENDED` carries the same
   `call_id` as the earlier `RING_DETECTED` and a **different**
   `event_id`.
7. Pulse GPIO4 again (a new call). Confirm the new `RING_DETECTED`'s
   `call_id=<B>` is different from `<A>`.
8. Pulse GPIO3 once for this second call, then leave the device idle.
   Pulse GPIO3 again with no active call: confirm it is ignored exactly
   as in step 3.
9. Start a third call (GPIO4 pulse, `call_id=<C>`) and this time do
   **not** pulse GPIO3 at all. After `kDevCallTimeoutMs` (60s by
   default), expect `[DEV RING] call timed out after 60000ms with no
   GPIO3 pulse; RING_ENDED enqueued call_id=<C> reason=timeout(local-only)`
   and confirm the backend still only ever sees a normal `RING_ENDED`
   payload for `<C>` (no `reason`/timeout field on the wire - see "Call
   session state machine" above).
10. Repeat step 9's setup, but this time pulse GPIO3 in the last few
    seconds before the timeout would fire. Confirm exactly one
    `RING_ENDED` is produced for that call (the button-triggered one; no
    second, timeout-triggered `RING_ENDED` follows it).
11. Disconnect Wi-Fi (or block the AWS IoT endpoint), start a call
    (GPIO4), end it (GPIO3), then restore connectivity. Confirm both
    `RING_DETECTED` and `RING_ENDED` are published on reconnect, **in
    that order**, with their original `event_id`s and shared `call_id`
    unchanged.
12. Confirm an `OPEN_DOOR` command (step 12 of the procedure above) still
    works correctly during an active simulated call, and that a call
    session in progress does not interfere with command processing or
    vice versa.

## Bench test history

Chronological, consolidated record of every real-hardware test run
during this phase. Earlier, superseded interpretations are corrected
inline rather than left standing separately.

1. **First boot: no association, no diagnostics.** `wifi=down` for a
   full 120s with no way to tell why `WiFi.begin()` wasn't succeeding.
   Added Wi-Fi event/action logging parity with `esp32-c3-dev-mqtt`.
2. **Retest with logging: both DEV environments failed identically.**
   `esp32-c3-dev-mqtt` - previously assumed to "already connect" from an
   unrelated earlier session - failed the same way on the same session,
   with the driver itself logging `wifi:sta is connecting, return
   error`. This exposed a real, shared coordination defect: the
   `DevMqttSmokeState` Wi-Fi coordinator reissued `WiFi.begin()` while a
   previous association attempt was still outstanding. **Fixed**: an
   explicit in-flight/timeout tracking mechanism
   (`wifiAssociationResult()`), mirroring the class's existing NTP
   handling.
3. **Retest after the coordination fix: driver error gone, association
   still failed.** `reason=2`/`202` (auth-stage rejection) persisted on
   the home network - the concurrent retry was not the sole cause. Also
   found and fixed an unrelated `uint32_t` underflow in "delay_ms=" log
   lines (display-only, no retry/backoff change).
4. **WPA2 test hotspot retest: a different failure mode.**
   `reason=201` (`no_ap_found`) against the hotspot vs. `reason=2`/`202`
   on the home network - neither root-caused. Added sanitized
   SSID/password byte-length + placeholder-match diagnostics and a
   controlled Wi-Fi scan to distinguish "wrong credential bytes,"
   "AP never seen," and "real match rejected."
5. **Pre-retest code review found three further defects**, fixed before
   ever flashing the diagnostics from step 4: the association timeout
   was armed before the (blocking) scan instead of at the real
   `WiFi.begin()`; a failed scan call was indistinguishable from one that
   validly found zero networks; and the credential diagnostic copied the
   Wi-Fi password onto the heap on every heartbeat tick.
6. **Fifth test: the button assembly correlated with Wi-Fi dropping -
   and the electrical assumption behind it was wrong.** With the
   ring-simulator button physically disconnected from GPIO20, the
   firmware associated cleanly to `Online` for the first time in this
   investigation; reconnecting it to GPIO20 dropped Wi-Fi again. This was
   initially read as "GPIO20 causes Wi-Fi to disconnect," but that
   reading does not hold up: the component wired there is a Linker
   Button module (active-HIGH, needs `VCC`/`GND`/`SIG`), while the
   firmware and this runbook had assumed a dry, active-LOW contact wired
   with `INPUT_PULLUP`. **Corrected framing**: the button assembly then
   in use through GPIO20 correlated reproducibly with the Wi-Fi drop.
   The exact physical cause of that drop was not isolated - not the
   GPIO20 pin specifically, not a UART silicon function, not the
   credential, and not either AP - only that this specific,
   electrically-mismatched assembly correlated with it. To continue the
   test with controlled variables, the simulator now uses
   GPIO4 and the module's correct electrical interface (this pass): a
   plain `INPUT` (no pull-up) read as active-high, matching the module's
   actual datasheet behavior.
7. **This pass, in addition to the electrical fix**: simplified the Wi-Fi
   scan to run exactly once per boot (a real test showed
   `WiFi.scanNetworks()` itself returning `-2` after several association
   attempts - removed the interval/failure-count rescan policy entirely,
   since the scan is a bench diagnostic, not part of the product); added
   the health/presence publish described in "Online status" above, after
   a real test reached local `Online` while the app still showed the
   device offline.

8. **Successful controlled GPIO4 validation after PR #20.** On an
   ESP32-C3 Super Mini, GPIO4 was held LOW through an approximately 10 kΩ
   resistor to GND and pulsed momentarily to 3V3. Wi-Fi connected, NTP
   synchronized, AWS IoT MQTT/mTLS connected, and a health report was
   published; afterward the app showed the device online. One pulse logged
   `valid press detected; RING_DETECTED enqueued`, followed exactly once by
   `publish confirmed count=1 remaining=0`. The event traversed AWS IoT,
   `telemetry_ingestion`, `push_sender`, FCM, and appeared correctly as an
   Android notification.

**Net effect:** the earlier GPIO20 attempts and Wi-Fi fixes remain the
technical history leading to the successful run. The controlled GPIO4 test
closes the 3B.8 chain without attributing the earlier failures to GPIO20,
credentials, or either AP, and without claiming validation of the Linker
Button.

## Validated state

**Phase 3B.8 is validated end to end on real hardware.** The run validated:

- the isolated `esp32-c3-dev-ring-simulator` environment;
- one controlled GPIO4 LOW→HIGH transition, debounce/coordinator behavior,
  exactly one `RING_DETECTED`, and exactly one confirmed MQTT/mTLS publish;
- AWS IoT ingestion, backend persistence/processing through
  `telemetry_ingestion`, `push_sender`, FCM, and notification receipt and
  presentation on Android; and
- the health report path making the device visible as online in the app.

This physical run did **not** validate the Linker Button's electrical
behavior or wiring, holding the button, repeated presses, offline replay or
physical preservation of the same `event_id`, a real Si3050/Si3018/Si3019
or analog intercom line, production ring detection, or GPIO4 as a production
assignment. It also did not validate audio, `CALL_STARTED`, `CALL_ENDED`,
off-hook, bidirectional calls, production firmware, real BLE onboarding, or
the complete full-screen/call UI. Automated tests still cover debounce,
repeated-edge, offline queue/replay, and stable-`event_id` behavior, but
those cases were not exercised physically in this run.

**This run predates the "Command processing" pass above** (command
subscription/`OPEN_DOOR` handling did not exist on this environment yet at
the time of this hardware run). `CommandHandler`/`RemoteCommandProcessor`
are themselves hardware-validated - on `esp32-c3-dev-mqtt`, see
`docs/mqtt-dev-smoke-test.md` - but this specific composition (ring
simulator + commands together) has not yet been retested on real hardware;
see "Command processing" above for exactly what remains open.

GPIO4 is only a provisional DEV overlay with `kSi3050PinPcmDrx` (DRX). The
overlap was safe for this test solely because the isolated simulator does
not compile or initialize Si3050 code and the chip was not connected. It
does not alter production pinout, authorize simultaneous button/DRX use, or
settle the final-board decision.

A future Linker Button evaluation is optional and separate from 3B.8.
Production must not depend on that module without its own electrical
validation.

### Call session addition (GPIO3/`RING_ENDED`/`call_id`): not yet hardware-validated

Everything above this subsection describes the GPIO4-only 3B.8 state that
**is** validated on real hardware. The call-session addition (GPIO3,
`RING_ENDED`, `call_id`, the safety timeout - see "Call session state
machine" above) is a separate, later, software-only pass:

- Implemented, compiled, and **native-tested only**
  (`test/test_dev_ring_event`, 16 tests covering the state machine, both
  buttons' debounce, the `call_id`/`event_id` contract, the safety
  timeout, the GPIO3/timeout race, publish ordering, and restart
  behavior).
- **Not yet run on any real ESP32-C3.** No real GPIO3 pulse, no real
  `RING_ENDED` publish, and no real backend/app handling of `RING_ENDED`
  or `call_id` has been exercised - see "Call session manual test
  procedure (GPIO3 + GPIO4, pending real-hardware run)" above for the
  pending runbook.
- `RING_ENDED` and `call_id` are, as of this pass, only defined in this
  firmware repository and in `docs/communication-protocol.md` as a
  **proposed** extension (section 16.1) - neither has been confirmed
  against a real backend implementation.
- GPIO3 is a second, equally provisional DEV-only overlay, this time with
  `kSi3050PinPcmDtx` (DTX), justified for exactly the same reason and
  under exactly the same constraints as GPIO4's DRX overlay above - see
  "Why GPIO4 and GPIO3, and why both are provisional".
- This pass does **not** claim that the Si3050 was tested, that a real
  intercom line's ringing was detected or its end observed, that the
  Linker Button module was validated, that GPIO3/GPIO4 are final
  production pin assignments, or that any physical door was opened.

## Connectivity recovery hardening (this pass)

A real bench run of the firmware described above (GPIO4/GPIO3 call
session, on top of the already-hardware-validated 3B.8 connectivity/
command stack) connected successfully, then lost connectivity and never
recovered on its own - only a manual reboot restored it. Sanitized log
excerpt (no credentials/endpoint/identity):

```text
[DEV RING] valid press detected; RING_DETECTED enqueued
[WARN] AWS IoT publish failed; session marked invalid (mqtt_err=-9)
[DEV RING] state online -> mqtt
hostByName(): DNS Failed
[DEV RING] network preflight dns=failed
[DEV RING] state mqtt -> dns
[DEV RING] state dns -> wifi
[DEV RING] wifi recovery requested
[DEV RING] wifi event=disconnected reason=8
[DEV RING] wifi recovery cooldown until_ms=710086
[DEV RING] Wi-Fi connect requested; next_attempt_ms=117111 delay_ms=0
[DEV RING] wifi event=disconnected reason=204
[DEV RING] wifi event=connected
[DEV RING] wifi event=disconnected reason=8
[DEV RING] Wi-Fi connect requested
[DEV RING] wifi event=disconnected reason=39 (timeout)
```

### Root cause

Two independent, compounding defects, both in code paths this repository
owns (not the AP/router):

1. **`ARDUINO_EVENT_WIFI_STA_CONNECTED` (L2 association only, before
   DHCP) was forwarded to `DevMqttSmokeState` identically to
   `ARDUINO_EVENT_WIFI_STA_GOT_IP`** (both logged as `wifi event=connected`
   and both set the same "success" flag). `DevMqttSmokeState` itself had a
   latent bug that made this genuinely dangerous rather than merely
   imprecise: a success signal (`wifiAssociationResult(nowMs, true)`)
   cleared the in-flight attempt immediately with no further safeguard -
   if `wifiConnected` (`WiFi.status()==WL_CONNECTED`, which the real
   ESP32 Arduino core only reports after `GOT_IP`) then never actually
   became true (association was only L2, or a later drop happened before
   DHCP completed), `actionIssued_` was left permanently `true` with no
   state transition ever pending to reset it - **no further `ConnectWifi`
   was ever issued again**, for that boot, without a manual reboot. Every
   `reason=8` (`WIFI_REASON_ASSOC_LEAVE`) in the log above is also, on its
   own, ambiguous: the ESP32 Wi-Fi driver reports that exact code both for
   an AP-initiated drop and for a disconnect the firmware requested
   itself (the `RecoverWifi` case's own `WiFi.disconnect()` call) - the
   log alone could not previously tell the two apart.
2. **The ESP32 Arduino core's own Wi-Fi auto-reconnect (enabled by
   default) was never disabled**, so it could retry association
   internally, independent of and racing against
   `DevMqttSmokeState`'s own explicit `ConnectWifi`/backoff cadence - a
   second, uncoordinated source of `connected`/`disconnected` events this
   firmware never accounted for.

A third, narrower defect was found and fixed while implementing the first
fix above: `DevMqttSmokeState`'s original design reset its Wi-Fi-retry
backoff to the floor on every re-entry into `WaitingForWifi` (via
`enter()`), including a merely momentary reassociation that dropped again
before ever reaching a stable connection - masking real instability
behind a backoff that kept restarting near-instantly instead of growing.

### Fixes

- **`DevMqttSmokeState`** (`src/dev/mqtt_smoke_state.h/.cpp`) gained a
  bounded "awaiting confirmed connect" window
  (`wifiConnectConfirmationPending()`): a success signal no longer goes
  fully idle immediately - if `wifiConnected` does not become genuinely
  true within this window, the attempt is now treated as failed and a
  fresh retry is scheduled automatically. This makes the state machine
  itself robust against a premature/unconfirmed success signal, not just
  the caller that produces one - defense in depth, not only a caller-side
  fix.
- **A dedicated Wi-Fi reconnection backoff**, separate from the
  per-stage (DNS/Time/MQTT) backoff those stages continue to use exactly
  as before: it grows only on an explicit failed/timed-out reconnect
  *attempt*, and is reset to its floor only by a full, genuinely stable
  success (`mqttResult(true)`, reaching `Online`) - never merely by
  re-entering `WaitingForWifi`. A fresh loss-of-connectivity transition
  still gets a prompt first retry (the deadline is reseeded from `nowMs`,
  both because that is the correct reaction to a just-detected real loss
  and to avoid a 32-bit `millis()` wraparound hazard on a long-untouched
  deadline - a real defect found and fixed during this same work, before
  it ever reached a committed state); the *growth* itself never resets on
  that transition alone. `retryDelayMs()`/`retryAtMs()` are now
  context-sensitive: they report this dedicated ladder while
  `state()==WaitingForWifi`, and the unchanged shared ladder otherwise.
- **`ARDUINO_EVENT_WIFI_STA_CONNECTED` and `..._GOT_IP` are now handled
  distinctly** in both `dev_ring_simulator_main.cpp` and
  `mqtt_smoke_main.cpp`: only `GOT_IP` is forwarded as a success signal;
  `STA_CONNECTED` is logged (`wifi event=associated awaiting_ip=true`)
  but never treated as "ready."
- **`WiFi.setAutoReconnect(false)`** is now called once in `setup()` in
  both DEV entry points, so this firmware's own `DevMqttSmokeState`-driven
  `WiFi.begin()` calls are the *only* source of reconnection attempts -
  no second, uncoordinated retry loop running inside the driver.
- **Disconnect origin is now logged**: a `wifiLocalDisconnectExpected`
  flag, set immediately before the `RecoverWifi` case's own
  `WiFi.disconnect()` call and consumed by the very next disconnect event,
  labels that event `origin=local_recovery` in the log; every other
  disconnect logs `origin=remote_or_unknown` - `reason=8` is no longer
  ambiguous between "the AP dropped us" and "we asked to disconnect."
  `wifiDisconnectReasonName()` also now names `WIFI_REASON_ASSOC_LEAVE`,
  `WIFI_REASON_HANDSHAKE_TIMEOUT`, `WIFI_REASON_AUTH_EXPIRE`, and
  `WIFI_REASON_BEACON_TIMEOUT` explicitly (previously only
  `NO_AP_FOUND`/`TIMEOUT` were named; everything else fell back to a bare
  numeric code).
- **Heartbeat log spam reduced**: the full sanitized credential/Wi-Fi-scan
  summary no longer repeats every 15s heartbeat while offline (nothing in
  it changes between repeats - the scan itself runs once per boot); it
  now repeats at most every 2 minutes. The terse heartbeat line gained
  `rssi=<dBm>` so signal strength stays visible at the normal cadence.
- **Boot reset-reason diagnostic** (`previous_reset=...`, already present
  since the original 3B.8 pass) now names an unrecognized
  `esp_reset_reason_t` value as `unknown` rather than `other`, matching
  this pass's intent precisely: distinguishing power-on/software/panic/
  watchdog/brownout resets from "we don't know" - never expands into a
  full stack/memory dump, and never logs anything sensitive.

### What is unchanged

- **The outbox remains RAM-only** (`MemoryEventOutbox`) - a reboot while
  an event is still queued still loses it. This pass fixes *ordinary*
  connectivity-loss recovery so a reboot is no longer *necessary* for
  that case; it does not add persistence. Durable (NVS/flash-backed)
  outbox persistence remains explicitly out of scope here and is tracked
  as future work in `CONTEXT.md` - wear, atomicity, corruption, capacity,
  and credential-adjacent storage concerns all need their own review
  before that is attempted.
- **Event identity/ordering are untouched**: `RING_DETECTED`/`RING_ENDED`
  JSON is still built and enqueued once, at the moment of the original
  GPIO edge/timeout (see "Call session state machine" above) - a retry
  during recovery only ever republishes the exact bytes already sitting
  in the outbox, never reconstructs the payload with a new timestamp.
  `event_id`/`call_id` are never regenerated on retry, and
  `publishPendingEvents()`'s existing strict-FIFO ordering (see "Call
  session state machine" > Publish ordering) is untouched by this pass -
  an old, offline-queued event still reaches the backend for history even
  after it is no longer fresh enough for a push notification; deciding
  that is explicitly the backend's policy to apply, not something this
  firmware invents.
- **GPIO4/GPIO3, the call-session state machine, and
  `DevCommandEnvironment`/command processing are completely untouched**
  by this pass - only the Wi-Fi/MQTT connectivity recovery layer changed.

### Validation

- All pre-existing native suites continue to pass unchanged (26 → 30
  tests in `test/test_dev_mqtt_state`, covering: a success signal that
  never resolves to a real connection eventually retries instead of
  wedging; a momentary reconnect-then-drop never resets the Wi-Fi backoff
  growth; only a full stable success resets it; and the ordinary Wi-Fi
  retry is never suppressed by an active recovery cooldown, which exists
  only to gate *escalating* to a second forced full recovery).
- `esp32-c3`, `esp32-c3-dev-mqtt`, and `esp32-c3-dev-ring-simulator` all
  compile against the real ESP32-C3 toolchain (confirms the newly
  referenced `WIFI_REASON_*` constants and `WiFi.setAutoReconnect()` are
  valid for the pinned `espressif32`/Arduino core version).
- **Not yet re-validated on real hardware.** The next physical test must
  reproduce a comparable connectivity loss (e.g. briefly power off the
  AP/router, or move the device out of range and back) without rebooting
  the ESP32, and confirm autonomous recovery - see the updated manual
  procedure below.

### Manual test addition: connectivity recovery (pending real-hardware run)

Extends the existing manual procedures above; run after a normal boot has
already reached `Online`.

1. Note the current `local_status=online` heartbeat line, then interrupt
   connectivity without touching the ESP32 itself - power off the Wi-Fi
   AP/router, or block the configured AWS IoT endpoint at the network
   level, for at least a few minutes.
2. Confirm the device logs a state regression (`state online -> ...`),
   Wi-Fi disconnect/reassociation attempts with `origin=` on each one, and
   - if the interruption lasts long enough to accumulate three
     connectivity failures - a `wifi recovery requested` /
     `wifi disconnect requested origin=local_recovery` cycle.
3. While still interrupted, trigger a GPIO4 pulse (and optionally a GPIO3
   pulse). Confirm the resulting event(s) are enqueued
   (`outbox_size` increases in the heartbeat line) even though nothing
   can be published yet.
4. Restore connectivity (power the AP back on, unblock the endpoint) -
   **do not power-cycle or reset the ESP32**.
5. Confirm the device recovers on its own: Wi-Fi reassociates, `got_ip`
   is logged (distinctly from the earlier `associated awaiting_ip=true`),
   DNS/MQTT re-establish, `network stable; wifi retry backoff reset` is
   logged, and the queued event(s) from step 3 are published
   (`outbox_size` returns to 0) with their **original** `event_id`s and
   `call_id`(s) - confirm on the backend/AWS IoT side that the
   `RING_DETECTED`/`RING_ENDED` payloads that eventually arrive still
   carry the timestamp from when the GPIO pulse actually happened, not
   from the moment connectivity was restored.
6. Only as a separate, explicit sub-case: power-cycle or reset the ESP32
   while an event is still genuinely queued (before step 4). Confirm the
   boot log's `previous_reset=` line, and confirm that queued event is
   lost (`outbox_size=0` after boot with no corresponding publish) -
   this is the documented, unchanged RAM-only outbox limitation, not a
   new regression.
