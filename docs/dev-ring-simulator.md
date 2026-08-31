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

**Validated state (see the consolidated record below):** 3B.8 is
implemented, compiled, unit-tested, and validated end to end on a real
ESP32-C3 Super Mini. The successful test used a controlled electrical
stimulus rather than the Linker Button: GPIO4 was held LOW through an
approximately 10 kΩ resistor to GND, then connected momentarily to 3V3.
Wi-Fi, NTP, and AWS IoT MQTT/mTLS completed; the health report made the
device appear online in the app; and the pulse produced exactly one event
and one confirmed publish before traversing the backend, FCM, and Android
notification path.

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
- The button only ever produces `RING_DETECTED`. It cannot and does not
  simulate `OFF_HOOK`, `CALL_STARTED`, `CALL_ENDED`, or any other audio/
  call-state event.
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

### Why GPIO4, and why this is provisional

The validated bench board (generic 4 MB ESP32-C3 Super Mini, see
`platformio.ini`/`CONTEXT.md`) exposes only 15 GPIOs total: 0-10 and
18-21.

| Pins | Committed to |
|---|---|
| 0, 1, 2, 3, **4**, 5, 6, 7, 8, 10 | Real Si3050 wiring (`src/intercom/si3050/si3050_pins.h`) - GPIO4 is the Si3050's DRX line (`kSi3050PinPcmDrx`) |
| 2, 8, 9 | BOOT/strapping pins - excluded |
| 18, 19 | Native USB D-/D+ (serial console) - reserved |
| 20, 21 | *Documentation-reserved only* for a future physical config/reset button and status LED (`kSi3050ReservedPinButton`/`kSi3050ReservedPinStatusLed`), neither implemented in any current code path. GPIO20 was tried and abandoned for this bench rig - see Bench test history. |

With no pin on this board free of *some* real or planned use, the
current choice is a **deliberate, explicit overlap with one real Si3050
pin: GPIO4** (`kSi3050PinPcmDrx`). This is safe **only** because
`esp32-c3-dev-ring-simulator` never compiles or initializes any
Si3050/RingDetector/PCM-clock code, and no Si3050 is physically attached
to the board while this bench test runs. `src/dev/dev_ring_simulator_config.h`
compile-time-asserts the button pin is exactly this one approved overlap
- not any other Si3050 pin, and not the BOOT/USB pins - so a future
change can never silently drift to an unreviewed pin. This is **not** a
production pin assignment and must be revisited once the Si3050 and the
final board (with the real config/reset button) are integrated together.
The real Si3050 pin map (`src/intercom/si3050/si3050_pins.h`) and
production firmware are completely untouched by this choice.

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
   `[DEV RING] button initialized gpio=4 mode=INPUT active=high
   module=linker`, and `[DEV RING] config=valid ssid_bytes=N
   password_bytes=N placeholder=false` (a `config=invalid` or
   `placeholder=true` here means the compiled binary does not hold the
   intended credential - stop and regenerate the secrets header before
   continuing).
5. Confirm `[DEV RING] wifi scan networks_found=N
   configured_ssid_found=true|false ...` right before the first
   `WiFi.begin()` - this scan runs exactly once per boot; a reboot is
   required to refresh it.
6. Wait for `[DEV RING] state ... -> online` (Wi-Fi → DNS → NTP → MQTT).
   If association fails, capture the `[DEV RING] wifi event=disconnected
   reason=N (...)` line(s) verbatim alongside the `config=`/`wifi scan`
   lines from steps 4-5.
7. Press the button once (do not hold). Expect, in order:
   - `[DEV RING] valid press detected; RING_DETECTED enqueued`
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

GPIO4 is only a provisional DEV overlay with `kSi3050PinPcmDrx` (DRX). The
overlap was safe for this test solely because the isolated simulator does
not compile or initialize Si3050 code and the chip was not connected. It
does not alter production pinout, authorize simultaneous button/DRX use, or
settle the final-board decision.

A future Linker Button evaluation is optional and separate from 3B.8.
Production must not depend on that module without its own electrical
validation.
