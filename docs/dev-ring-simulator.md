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
audio, off-hook, in-call, or any other intercom state - only the initial
ring detection signal. See Scope and safety below.

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
  provisioning/BLE, Wi-Fi credential handling, or the production AWS
  IoT composition root. It reuses the DEV MQTT smoke environment's
  connectivity bring-up class, `DevMqttSmokeState`
  (`src/dev/mqtt_smoke_state.*`, already validated on real hardware - see
  `docs/mqtt-dev-smoke-test.md`), rather than duplicating that state
  machine.
- Publishes **only** through the existing production contract: the topic
  returned by `MqttTopics::eventsIngest()`, `Esp32AwsIotTransport` (AWS IoT
  Basic Ingest, mTLS), and the existing `IEventOutbox`
  (`MemoryEventOutbox`) - see `src/dev/dev_ring_event.*`. No second MQTT
  client, no parallel/ad hoc topic.
- The button only ever produces `RING_DETECTED`. It cannot and does not
  simulate `OFF_HOOK`, `CALL_STARTED`, `CALL_ENDED`, or any other audio/
  call-state event.
- Same DEV-only credential model as `esp32-c3-dev-mqtt`: certificates load
  into transient `MemoryStore` (not NVS), never logged.

## Wiring diagram

```
                 ESP32-C3 Super Mini
                +---------------------+
                |                     |
                |   GPIO20 (RX0) o----+-----+
                |                     |     |
                |                 GND o--+  |
                +---------------------+  |  |
                                         |  |
                                         |  |
                                     +---+--+---+
                                     |  momentary |
                                     |   push     |
                                     |  button    |
                                     +------------+
```

- One leg of a momentary push button to **GPIO20**, the other leg to
  **GND**.
- No external resistor needed: `pinMode(GPIO20, INPUT_PULLUP)` enables the
  ESP32-C3's internal pull-up. The pin reads **HIGH when released** and
  **LOW when pressed** (active-low).

### Why GPIO20, and why this is provisional

The validated bench board (generic 4 MB ESP32-C3 Super Mini, see
`platformio.ini`/`CONTEXT.md`) exposes only 15 GPIOs total: 0-10 and
18-21. Of those:

| Pins | Committed to |
|---|---|
| 0, 1, 2, 3, 4, 5, 6, 7, 8, 10 | Real Si3050 wiring (`src/intercom/si3050/si3050_pins.h`) |
| 9 | BOOT/download-mode strap |
| 18, 19 | Native USB D-/D+ (serial console) |
| 20, 21 | *Documentation-reserved only* for a future physical config/reset button and status LED (`kSi3050ReservedPinButton`/`kSi3050ReservedPinStatusLed`) - **neither is implemented in any current code path** (`Esp32ButtonInput`/`Esp32StatusIndicator` remain unassigned stubs) |

That leaves no genuinely free GPIO on this specific board. The explicit,
user-approved decision for this DEV-only environment is to temporarily
reuse **GPIO20**, scoped exclusively to `esp32-c3-dev-ring-simulator` (see
`src/dev/dev_ring_simulator_config.h`, which also compile-time-asserts
this pin never collides with a real Si3050/BOOT/USB pin). This is **not**
a production pin assignment. It must be revisited once the Si3050 and the
final board - including the real physical config/reset button - are
integrated together on the same physical unit; at that point GPIO20 may
no longer be free for this simulator and a different (or no) GPIO should
be chosen.

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

## Manual flash and test procedure (future, on real hardware)

This has **not yet been run on real hardware** - see Honest status below.
The intended procedure, mirroring `docs/mqtt-dev-smoke-test.md`'s DEV MQTT
smoke test:

1. Wire the button as shown above.
2. Generate `include/interbridge_dev_secrets.h` for a DEV device identity
   (a dedicated `device_id`, distinct from the DEV MQTT smoke harness's,
   is recommended so the two bench firmwares don't collide on the same
   AWS IoT Thing).
3. `pio run -e esp32-c3-dev-ring-simulator -t upload` and attach the
   serial monitor.
4. Confirm the boot log line `[DEV RING] button initialized gpio=20
   (INPUT_PULLUP, active-low)`.
5. Wait for `[DEV RING] state ... -> online` (Wi-Fi → DNS → NTP → MQTT
   connect, identical cascade to the DEV MQTT smoke test).
6. Press the button once. Expect, in order:
   - `[DEV RING] valid press detected; RING_DETECTED enqueued`
   - `[DEV RING] publish confirmed count=1 remaining=0`
7. Confirm on the backend/AWS IoT side that exactly one `RING_DETECTED`
   event arrived for that `device_id`, with a fresh `event_id` and a
   populated ISO-8601 `timestamp` (NTP sync should already have completed
   by step 5).
8. Press and hold: confirm no repeat event while held.
9. Release, then press again: confirm a second `RING_DETECTED` with a
   **different** `event_id`.
10. Disconnect Wi-Fi (or block the AWS IoT endpoint) while leaving the
    board powered, press the button, and confirm
    `[DEV RING] offline; event queued, awaiting reconnection` - then
    restore connectivity and confirm the queued event is published on
    reconnect with the **same** `event_id` it was enqueued with.

## Event and topic

- **Topic**: `MqttTopics::eventsIngest()` - the same AWS IoT Basic Ingest
  topic (`$aws/rules/interbridge_dev_ingest_rule/interbridge/{device_id}/events`
  for this DEV rule name) production `RING_DETECTED` events use.
- **Payload**: the standard `DeviceEvent` v1 contract
  (`src/protocol/messages.h`):

  ```json
  {"protocol_version":1,"device_id":"ib-...","event":"RING_DETECTED","event_id":"evt-...","timestamp":"2026-08-29T12:00:00Z"}
  ```

  `timestamp` is only present once NTP has completed (same convention as
  every other `DeviceEvent` in this codebase - see `messages.h`); it is
  omitted entirely if the clock is not yet trustworthy at the moment of
  the press.
- **QoS**: `AtLeastOnce` (QoS 1), matching `main.cpp`'s production event
  publishing.

## Offline behavior and replay

A press always enqueues into `MemoryEventOutbox` first, regardless of
connectivity - the button-to-outbox path
(`DevRingEventCoordinator::update()`, `src/dev/dev_ring_event.*`) never
touches the transport directly. Publishing is a separate step
(`publishPendingEvents()`) that only runs once `transport.isConnected()`
is true, and only dequeues an entry after a **successful** publish - an
offline or failed attempt leaves the entry queued untouched, with the
same `event_id`, for the next attempt. This exactly mirrors
`main.cpp`'s production `updateNetwork()` outbox loop; see
`src/dev/dev_ring_event.cpp`.

The outbox is **RAM-only** (`MemoryEventOutbox`, not
`PersistentEventOutbox`) - a reboot while an event is still queued loses
it, the same DEV-only tradeoff `mqtt_smoke_main.cpp` already accepts for
its credentials (see its own "DEV-only transient composition" comment).
This is acceptable for a bench convenience tool; production's own outbox
path is unaffected (this environment does not compile `main.cpp` at all).

## Dependency on Phases 3B.6/3B.7

This phase only makes the **firmware** publish a real `RING_DETECTED`
event. Turning that into a phone notification requires the rest of the
pipeline, tracked separately (see `docs/roadmap-3b.md`):

- **Phase 3B.6** (backend FCM sender): `telemetry_ingestion` invokes
  `push_sender`, which delivers the push notification via Firebase Cloud
  Messaging. **Implemented and deployed in DEV** - the backend has
  accepted a real event end-to-end and recorded `Sent=1` for it.
- **Phase 3B.7** (notification preference application): the backend
  applies the user's/app's notification preferences before deciding
  whether/how to notify. **Implemented and deployed in DEV.**

With 3B.6/3B.7 now deployed, this simulator's remaining, un-closed gap is
the **firmware side of the loop**: no real button on real hardware has
yet triggered that pipeline. The `Sent=1` confirmation above came from a
backend/synthetic test event, not from a physical press on a flashed
board - see Honest status below. The minimal slice of **Phase 3B.9**
needed to actually display a data-only FCM notification on Android is in
progress (mobile repo); the full call UI is separate future work.

## Honest status

**Implemented and compiled. Native tests pass locally; this update also
fixes two real defects found by CI on the previous commit (a dangling
reference into a temporary `std::vector` returned by
`IEventOutbox::pending()`, and test helpers that drove
`DevRingButtonController` directly instead of through
`DevRingEventCoordinator::update()`, silently skipping the outbox enqueue
and then calling `.back()` on an empty container - see
`test/test_dev_ring_event/test_main.cpp`). Whether this fix is actually
green on GitHub Actions CI must be confirmed by that run itself - this
document does not assert a passing CI result on its own.**

- Native unit tests (`test/test_dev_ring_button`, `test/test_dev_ring_event`)
  cover: one press → exactly one `RING_DETECTED`; holding the button
  never repeats; releasing and pressing again yields a new, different
  `event_id`; `event_id` matches `^evt-[0-9a-f]{32}$`; a failed publish
  attempt followed by a retry preserves the exact same `event_id` and
  payload; an offline press enqueues and a later reconnect replays it;
  `timestamp` is only present when the clock is valid; the JSON stays
  contract-compatible. All device-id fixtures used in these assertions
  are themselves asserted valid against the real `isValidDeviceId()`
  contract (`ib-` + exactly 32 lowercase hex chars) rather than using an
  arbitrary placeholder string.
- `pio run -e esp32-c3-dev-ring-simulator` compiles successfully, with no
  new warnings.
- `pio run -e esp32-c3`, `pio run -e esp32-c3-dev-mqtt`,
  `pio run -e esp32-c3-si3050-clock-probe`, and
  `pio run -e esp32dev-si3050-clock-meter` all still compile successfully
  and are unaffected in source (only `platformio.ini` gained one new
  exclusion line per environment to keep `dev_ring_simulator_main.cpp`
  out of them) - confirming this DEV-only addition does not alter any
  other build.
- **Not yet done**: flashing a real board, wiring the physical button,
  and running the manual procedure above. GPIO20's electrical behavior
  under `INPUT_PULLUP` on this specific module, the end-to-end AWS IoT
  delivery, and the offline/reconnect replay have only been exercised
  through native fakes (`FakeDevRingButtonInput`, `FakeDeviceTransport`),
  not real hardware - this is true even though 3B.6/3B.7 are now deployed
  in DEV, since neither of those exercised a real physical button.
