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
  (`src/dev/mqtt_smoke_state.*`, previously validated on real hardware for
  several other scenarios - see `docs/mqtt-dev-smoke-test.md` - though a
  real Wi-Fi-association coordination defect in it was only found via a
  later `esp32-c3-dev-ring-simulator`/`esp32-c3-dev-mqtt` retest, see
  below), rather than duplicating that state machine.
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

## Manual flash and test procedure (retest pending with the new Wi-Fi diagnostics)

This has been run on real hardware four times, and Wi-Fi association has
not yet succeeded on any attempt - see all four "Real bench observation"
sections below. A further retest with this pass's SSID/password/scan
diagnostics is still needed; see Honest status. The intended procedure,
mirroring `docs/mqtt-dev-smoke-test.md`'s DEV MQTT smoke test:

1. Wire the button as shown above.
2. Generate `include/interbridge_dev_secrets.h` for a DEV device identity
   (a dedicated `device_id`, distinct from the DEV MQTT smoke harness's,
   is recommended so the two bench firmwares don't collide on the same
   AWS IoT Thing).
3. `pio run -e esp32-c3-dev-ring-simulator -t upload` and attach the
   serial monitor.
4. Confirm the boot log lines `[DEV RING] previous_reset=...` and
   `[DEV RING] button initialized gpio=20 (INPUT_PULLUP, active-low)`.
4a. Confirm `[DEV RING] config=valid ssid_bytes=N password_bytes=N
    placeholder=false` right after boot - `config=invalid` or
    `placeholder=true` here means the compiled binary does not actually
    hold the credential the operator intended, before Wi-Fi is even
    attempted. Compare `ssid_bytes=N` against the real SSID's expected
    UTF-8 byte length (an apostrophe or accented character changes this
    from the plain character count).
4b. Watch for `[DEV RING] wifi scan networks_found=N
    configured_ssid_found=true|false rssi=N channel=N auth=... scan_age_ms=N`
    right before the first `WiFi.begin()`. `configured_ssid_found=false`
    with `networks_found` clearly non-zero means the ESP32's own scan
    never saw a beacon for that exact SSID (out of range, wrong band, or
    the configured SSID string does not match reality) - a strong signal
    before ever attempting association.
4c. Watch for `[DEV RING] Wi-Fi connect requested; next_attempt_ms=...`
    immediately after, and then either `[DEV RING] wifi event=connected`
    + `[DEV RING] wifi event=got_ip`, or a `[DEV RING] wifi
    event=disconnected reason=N (...)` line naming the actual disconnect
    reason. A single `Wi-Fi connect requested` line should appear per
    attempt (never a burst while one is still outstanding); if a
    disconnect reason keeps recurring, that reason code - not a repeat
    concurrent-retry - is the thing to diagnose. If Wi-Fi still does not
    associate, capture the reason code(s) verbatim alongside the
    `config=`/`wifi scan` lines from steps 4a/4b.
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

## Real bench observation: first boot never associated with Wi-Fi

A real bench test (clean build, fresh upload, the same ESP32-C3 board and
Wi-Fi network `esp32-c3-dev-mqtt` was also being tested against) produced
only:

```
[DEV RING] local_status=wifi wifi=down time=pending mqtt=down outbox_size=0 uptime_s=120
```

repeated at every 15s heartbeat, for a full 120s: Wi-Fi never associated.
At the time, `esp32-c3-dev-mqtt` had not itself been re-confirmed on this
specific bring-up path either - see "Real bench observation: retest
reveals a shared concurrent-retry defect" below, which corrects this.

**What the comparison against `mqtt_smoke_main.cpp` found (at this point
in the investigation):** the connectivity bring-up *logic* itself looked
equivalent - both files drive the same `DevMqttSmokeState` state machine,
call `WiFi.mode(WIFI_STA)` + `WiFi.begin()` only on its `ConnectWifi`
action, use the identical retry/backoff policy, call
`WiFi.disconnect(false, false)` only on `RecoverWifi` (radio stays on,
credentials are kept), never call `ESP.restart()`, and build with
identical flags/board. **This comparison turned out to be incomplete** -
see below for the actual defect it missed.

**What was also missing at this point: observability.** The simulator had
no `WiFi.onEvent()` handler and none of `mqtt_smoke_main.cpp`'s per-action
log lines (`Wi-Fi connect requested`, DNS/MQTT retry timing, state
transitions, `wifi recovery requested`) or boot diagnostics
(`previous_reset=...`). The single 15s heartbeat could show `wifi=down`
but had no way to reveal *why*. A first pass added that parity, copying
`mqtt_smoke_main.cpp`'s own logging pattern verbatim (`onWifiEvent()`,
`wifiDisconnectReasonName()`, `resetReasonName()`, and a log line for
every `DevSmokeAction`) into `dev_ring_simulator_main.cpp` - see below for
what the resulting retest actually showed.

## Real bench observation: retest reveals a shared concurrent-retry defect

With the Wi-Fi diagnostics above in place, a hardware retest of
**both** `esp32-c3-dev-ring-simulator` **and** `esp32-c3-dev-mqtt` showed
**the exact same Wi-Fi association failure on both environments**:

```
[DEV RING] wifi event=disconnected reason=2
[DEV RING] wifi event=disconnected reason=202
[DEV RING] Wi-Fi connect requested; ... delay_ms=16000
```

and, on `esp32-c3-dev-mqtt`:

```
wifi:sta is connecting, return error
WiFiSTA.cpp begin(): connect failed!
```

**This corrects the earlier framing above and in the original 3B.8
work: `esp32-c3-dev-mqtt` had not actually been re-confirmed working on
this exact bring-up path at the time - it was only assumed to be fine
because it had connected successfully in an earlier, separate bench
session.** SSID/password/network are **not** ruled out as contributing
factors just because this is a coordination bug - reason 2 (auth expire)
and reason 202 can also occur with a wrong/expired credential; that
specific question needs to be re-evaluated after the fix below, on a
clean retest, not assumed either way from this evidence alone.

**The concrete, code-level defect this evidence exposed:** the shared
`DevMqttSmokeState` coordinator did not model a Wi-Fi association attempt
as asynchronous/in-flight the way it already did for NTP - `!wifiConnected`
alone (i.e. `WiFi.status() != WL_CONNECTED`) was being treated as
sufficient authorization to reissue `WiFi.begin()` once the ordinary
backoff deadline elapsed, with no way to tell "still trying" apart from
"gave up." The ESP32 Wi-Fi driver actively rejects (and effectively
restarts) a `WiFi.begin()` call issued while a previous attempt is still
outstanding - exactly `wifi:sta is connecting, return error` /
`WiFiSTA.cpp begin(): connect failed!` above - so a fast-enough backoff
could keep interrupting an association that might otherwise have
succeeded, on **both** DEV environments equally, since they share this
same coordinator class.

**Fix**: `DevMqttSmokeState` now tracks a Wi-Fi association attempt as
explicitly in flight (mirroring the existing NTP/SNTP in-flight pattern),
gates `ConnectWifi` on it, and only schedules the next backoff-governed
retry once the attempt actually resolves - via a real
`ARDUINO_EVENT_WIFI_STA_CONNECTED`/`GOT_IP` (success) or
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED` (failure) event forwarded from the
caller, or a configurable, separate association timeout if neither
arrives. Both `mqtt_smoke_main.cpp` and `dev_ring_simulator_main.cpp` now
forward those events (recorded as a minimal signal in the Wi-Fi event
callback, which runs on its own task, and only turned into an actual
`DevMqttSmokeState::wifiAssociationResult()` call from the ordinary,
single-threaded `loop()`, to avoid a real concurrency hazard) into the
same, single, shared coordinator - no state logic is duplicated between
the two mains. See `src/dev/mqtt_smoke_state.*` and its native tests
(`test/test_dev_mqtt_state`) for the full mechanism.

**This fixes the concurrent-retry coordination bug; it does not yet
prove Wi-Fi will associate.** Reason 2/202's specific underlying cause
(credential, AP-side rejection, signal, or something else) has not been
separately diagnosed and **must be re-evaluated on the next hardware
retest**, now that the firmware itself will no longer interrupt its own
association attempts. No fix has been guessed at for GPIO20 either (still
considered a possible contributing factor only in the sense that it
remains unvalidated, not because any evidence ties it to this specific
failure) - see "Why GPIO20" above.

## Real bench observation: concurrent retry gone, auth still fails (reason=2/202)

A further hardware retest of the concurrent-retry fix above confirmed it
worked as intended: **the `wifi:sta is connecting, return error` /
`WiFiSTA.cpp begin(): connect failed!` driver errors did not recur.**
`ConnectWifi` is no longer reissued while a previous association attempt
is still outstanding - the coordination bug is fixed.

**Association still fails, with the same reason codes as before (2, 202)
even without the concurrent retry.** This means the concurrent-retry bug
was not the (or not the only) cause of the original association failure.
Reason 2/202 - most consistent with an authentication/credential-level
rejection - has not been root-caused yet and is planned to be isolated
next using a dedicated WPA2 test hotspot (an access point with known-good,
controlled credentials, separate from the production/office network this
board was tested against), to determine whether the failure is
credential-specific, AP-specific, or something else entirely. Do not
conclude the underlying association problem is fixed - only the
concurrent-retry coordination defect is.

The same retest also surfaced a second, independent, diagnostic-only bug:
delay-until-next-retry log lines showed impossible values
(`delay_ms=4294967291`, `delay_ms=4294967294`) - a `uint32_t` underflow
from computing `retryAtMs() - now` with plain unsigned subtraction when
the deadline was already at or slightly past `now` (which is guaranteed
by construction the moment `ConnectWifi` fires, since that only happens
once `deadlineReached()` is already true). Fixed with a new
`DevMqttSmokeState::millisUntil(deadlineMs, nowMs)` helper that saturates
at 0 and stays correct across the `millis()` wraparound, used everywhere
a "delay_ms=" diagnostic is logged in both DEV mains. **This is a
display-only fix** - it does not change any retry/backoff timing or
policy, only how the remaining time is computed for logging. See
`test_millis_until_saturates_and_is_wrap_safe` in
`test/test_dev_mqtt_state` for future-deadline, already-passed-deadline,
and wraparound coverage.

## Real bench observation: WPA2 test hotspot isolates a different failure mode (reason=201)

A further retest against a dedicated WPA2 test hotspot (a personal
iPhone's Personal Hotspot, "Henrique's iPhone") produced a **different**
disconnect reason than the home network: **`reason=201`
(`WIFI_REASON_NO_AP_FOUND`)** - the ESP32 never saw an access point
matching the configured SSID at all, as opposed to reason 2/202 (an
authentication-stage rejection) on the home network, which persisted
unchanged.

**This does not yet distinguish "the configured SSID string is wrong"
from "the ESP32 genuinely never received a beacon for that network"** -
both would produce `no_ap_found`, and the diagnostics available at the
time (Wi-Fi association events, retry timing) could not tell them apart.
Plausible, still-unconfirmed explanations include the hotspot not
broadcasting on a channel/band the ESP32-C3 scans (2.4 GHz only),
being out of range, a typo when the credentials header was generated, or
some other credential-pipeline issue. **This finding is exactly why this
pass adds the sanitized SSID/password byte-length + placeholder check and
the controlled Wi-Fi scan (see below)** - together they can now show
whether the compiled binary actually holds the SSID/password bytes the
operator intended (length/placeholder facts, never the values) and
whether the ESP32's own scan sees the target AP at all, and with what
signal/channel/auth type, *before* any association is even attempted.

**Nothing here confirms the credential or either access point is
correct.** Reason 2/202 on the home network remains an unexplained
authentication failure; reason=201 on the test hotspot remains an
unexplained "AP not found." A further hardware retest with the new
diagnostics below is required before drawing a conclusion either way.

## Wi-Fi config and scan diagnostics (SSID/password length, placeholder check, controlled scan)

To close the gap the observation above exposed, both DEV mains now log,
sanitized, never including the raw SSID/password value or any other
network's name:

- **At boot, before every `WiFi.begin()`, and in the heartbeat while
  Wi-Fi is down** (so this is visible even if the serial monitor is
  attached well after boot and missed the earlier prints):
  `config=valid|invalid ssid_bytes=N password_bytes=N placeholder=true|false` -
  `valid` requires both fields non-empty and neither still equal to
  `include/interbridge_dev_secrets.example.h`'s placeholder text;
  `placeholder=true` means at least one field is still the unmodified
  example value (the header was generated but not actually filled in, or
  never regenerated after a `git clean`/fresh checkout).
- The same three log points also repeat the **last known Wi-Fi scan
  summary**: `networks_found=N configured_ssid_found=true|false rssi=N
  channel=N auth=... scan_age_ms=N` - RSSI/channel/auth are only ever the
  configured SSID's own values (if it was found); no other network's SSID
  is ever logged or retained.
- **The scan itself never runs before every association attempt** (it is
  a multi-second blocking operation - see `WiFi.scanNetworks()`'s default
  synchronous mode - and repeating it every retry would meaningfully slow
  down reconnection). It runs once on the very first `ConnectWifi`, and
  is repeated only after a 5-minute interval or 5 consecutive explicit
  association failures accumulate - see `kWifiRescanIntervalMs`/
  `kWifiRescanFailureThreshold` in both mains. A scan is always fully
  synchronous with (and strictly precedes) the `WiFi.begin()` call in the
  same `ConnectWifi` handler - by construction, the two can never overlap.
- The pure comparison/formatting logic (`diagnoseCredentialField()`,
  `summarizeCredentialConfig()`, `summarizeWifiScan()`, and both line
  formatters) lives in a new shared, native-testable module,
  `src/dev/dev_wifi_diagnostics.*`, used unmodified by both
  `mqtt_smoke_main.cpp` and `dev_ring_simulator_main.cpp` - only the
  Arduino-only glue (reading `WiFi.SSID()`/`RSSI()`/`channel()`/
  `encryptionType()`, calling `WiFi.scanNetworks()`, and the actual
  `Serial.print` calls) is duplicated per main, same pattern as
  `DevMqttSmokeState`'s other callers. See
  `test/test_dev_wifi_diagnostics` for the native coverage, including two
  tests that run distinctive marker SSID/password strings through the
  entire pipeline and assert they never appear in the formatted output.
- `scripts/generate_dev_secrets_header.ps1` now also rejects an SSID or
  password that still exactly matches the example header's placeholder
  text, reports only the generated SSID/password byte lengths (never the
  values) after writing, and validates that all seven expected macros
  appear exactly once in the file before it is written.

**This is diagnostics only - no retry/backoff policy changed, and no
credential/AP conclusion is drawn by this pass.** A hardware retest with
these diagnostics against both the WPA2 test hotspot and the home network
is still required.

## Honest status

**Implemented and compiled. `esp32-c3-dev-ring-simulator` has been
flashed and booted on real hardware four times, and Wi-Fi has not yet
associated on any attempt - see all four "Real bench observation"
sections above. The concurrent-retry coordination defect in the shared
`DevMqttSmokeState` (both DEV mains reissuing `WiFi.begin()` while a
previous attempt was still outstanding) is confirmed fixed - the
driver-level `wifi:sta is connecting, return error` error is gone.
Association still fails: reason 2/202 (auth-stage rejection) persists
unchanged on the home network, and a fourth retest against a dedicated
WPA2 test hotspot produced a *different* failure, reason=201
(`no_ap_found`) - the ESP32 never saw that access point at all. Neither
result has been root-caused. This pass adds sanitized SSID/password
length + placeholder-detection diagnostics and a controlled, rate-limited
Wi-Fi scan (reported before every `WiFi.begin()`, at boot, and in the
heartbeat while Wi-Fi is down) specifically to make the next retest able
to distinguish "wrong credential bytes reached the binary" from "the
ESP32 genuinely never received a beacon for that SSID" from "a real
match was rejected at authentication" - it does not itself draw either
conclusion. SSID/credential/AP correctness remain explicitly not
confirmed either way. A separate, diagnostic-only `uint32_t` underflow in
the "delay_ms=" log lines (`4294967291`, `4294967294` observed) is also
fixed - display-only, no retry/backoff policy changed. Button behavior,
MQTT connectivity, and end-to-end event delivery from a real physical
press remain unvalidated on hardware.**

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
- Native unit tests (`test/test_dev_mqtt_state`) cover the new Wi-Fi
  association in-flight tracking shared by both DEV mains: `ConnectWifi`
  is issued exactly once per attempt and never reissued by rapid updates
  while one is pending; an explicit success signal ends the attempt and
  lets the ordinary cascade advance normally; an explicit failure signal
  ends the attempt and schedules the next retry (never before its own
  backoff deadline); a stuck attempt with no event at all is abandoned by
  its own separate, configurable timeout and allows a later retry; that
  timeout is `millis()`-wraparound-safe; the existing Wi-Fi interface
  recovery ladder (`RecoverWifi`) still works and its own reconnect
  attempt is equally protected from reissue; and a burst of repeated
  failure signals for the same attempt never causes more than one
  `WiFi.begin()` at the single scheduled retry.
- Native unit tests (`test_millis_until_saturates_and_is_wrap_safe` in
  `test/test_dev_mqtt_state`) cover the delay-display fix: an ordinary
  future deadline, a deadline already reached (saturates at 0 instead of
  underflowing), and both directions of the `millis()` wraparound.
- Native unit tests (`test/test_dev_wifi_diagnostics`, 9 tests) cover the
  new Wi-Fi config/scan diagnostics: placeholder detection, correct byte
  lengths, a config summary that is only `valid` when both fields are
  non-empty and non-placeholder, a scan summary that correctly finds/does
  not find the configured SSID among fake scan results (including
  RSSI/channel/auth extraction), and - run through the entire pipeline
  with distinctive marker SSID/password/network-name strings - that the
  formatted diagnostic lines never contain any of those secret/name
  values, only the derived facts about them.
- `pio run -e esp32-c3-dev-ring-simulator` and `pio run -e esp32-c3-dev-mqtt`
  compile successfully, with no new warnings.
- `pio run -e esp32-c3`, `pio run -e esp32-c3-si3050-clock-probe`, and
  `pio run -e esp32dev-si3050-clock-meter` all still compile successfully
  and are unaffected in source - confirming this fix does not alter any
  other build.
- **Not yet done**: a hardware retest with the new SSID/password/scan
  diagnostics (against both the WPA2 test hotspot and the home network),
  isolating the reason=201 and reason=2/202 root causes, wiring the
  physical button, and running the rest of the manual procedure above.
  GPIO20's electrical behavior under `INPUT_PULLUP` on this specific
  module, the end-to-end AWS IoT delivery, and the offline/reconnect
  replay have only been exercised through native fakes
  (`FakeDevRingButtonInput`, `FakeDeviceTransport`, direct
  `DevMqttSmokeState` calls), not real hardware - this is true even
  though 3B.6/3B.7 are now deployed in DEV, since neither of those
  exercised a real physical button or this
  specific Wi-Fi coordination path.
