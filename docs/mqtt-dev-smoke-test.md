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
TLS stream operations use the configured one-second timeout, while the complete
AWS IoT TLS handshake has its own ten-second limit.

A valid command publishes ACCEPTED immediately, but its terminal response is
always deferred to the next main-loop iteration - never attempted in the same
call, even when ACCEPTED itself just published successfully. Two back-to-back
QoS 1 publishes with no `transport.poll()` in between (which happens naturally
between separate loop iterations) is what caused the terminal publish to fail
intermittently on real hardware even when ACCEPTED succeeded. Any response that
fails to publish (immediately or on a later drain) is queued in a small bounded
RAM outbox (`RemoteCommandProcessor`, `kMaxOutboxSize = 2`) instead of being
dropped, and the harness explicitly tears the MQTT/TLS session down
(`transport.disconnect()`) as soon as it observes the session invalidated, even
if Wi-Fi and NTP time remain fine - a publish/subscribe/poll failure alone is
now treated as an untrustworthy session. `Esp32AwsIotTransport` also allocates a
fresh `WiFiClientSecure` on every reconnect rather than reusing the previous
socket/TLS object. Each main-loop iteration attempts to publish at most one
response - starting a brand-new command, or draining one queued item - never a
burst; the outbox drains oldest-first, ACCEPTED before its terminal response,
once the harness reconnects and resubscribes, and a retry never re-invokes
`CommandHandler`. It never evicts an already-pending response to make room for a
new one - `kMaxOutboxSize` is a backstop for the ordinary "never start a new
command while the outbox is non-empty" invariant being violated, not working
capacity for several commands.

Diagnostic log lines distinguish exactly why a terminal response is pending -
never using "publish failed" wording for a response that was simply deferred
and never attempted:

```text
terminal deferred (queued for next iteration) seq=N code=...   ACCEPTED just published; terminal not attempted yet
terminal queued behind pending ACCEPTED seq=N code=...         ACCEPTED itself failed; terminal not attempted yet
terminal publish failed; still queued seq=N code=...           a real publish attempt for the terminal happened and failed
terminal published seq=N code=...                              the terminal actually published
```

The transport layer's own `AWS IoT publish failed; ... (mqtt_err=N)` log line
remains the evidence of an actual MQTT-level failure; the lines above report
the `RemoteCommandProcessor`-level outcome and never repeat that wording for a
response that was only deferred, not attempted. Log lines add a local `seq`
counter to correlate a command's ACCEPTED/terminal lines without ever logging
`command_id`. The outbox is RAM-only: a reboot loses any response still queued
at that moment (the command itself was already handled exactly once; only its
delivery confirmation to the backend is at risk, and the backend must treat a
republished ACCEPTED/terminal for the same `command_id` idempotently per
`docs/communication-protocol.md` section 20.1).

The local heartbeat reports current state plus free/minimum heap and remaining
loop-task stack. Boot diagnostics report the previous reset reason and only
whether Wi-Fi configuration is present. Wi-Fi driver events include disconnect
reason codes. The state machine authorizes one `WiFi.begin()` per attempt, with
capped backoff and the next deadline logged;
it deliberately leaves the interface running between retries so bench testing can
observe the driver's normal, non-aggressive recovery behavior. External network
outages never trigger a timed restart.

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

## Real bench observation: DNS/NTP retry timing

A real bench boot logged DNS failing repeatedly for roughly 78 seconds despite
Wi-Fi already being up, then `time sync requested` repeating until the serial
monitor disconnected; a subsequent reboot connected normally. Two different
findings came out of investigating this, and only one produced a code change -
this PR does not otherwise touch DNS/NTP:

- **DNS**: `ResolveDns` calls the blocking `WiFi.hostByName()` and immediately
  reports the boolean result back through `dnsResult()`, which schedules the
  next bounded exponential retry (`DevMqttSmokeState::scheduleRetry()`) only
  on that reported failure. There is no code path that could reissue a DNS
  attempt while a previous one is still outstanding - each attempt is
  synchronous and always reports before the next is scheduled. ~78 s is
  consistent with several exponential-backoff DNS retries (roughly 1, 2, 4,
  8, 16, 32 s) over a genuinely slow/unavailable resolver path after
  association, not a code defect. No change was made here - inventing a fix
  without evidence of a bug would just add risk.
- **NTP**: unlike DNS, an SNTP synchronization attempt is asynchronous -
  `configTime()` kicks it off and completion arrives later via
  `sntp_set_time_sync_notification_cb()`. `DevMqttSmokeState::update()`'s
  `WaitingForTime` branch only checked `timeValid` and the backoff deadline
  before reissuing `ConfigureTime` - it had no way to know whether a
  previous attempt might still be in flight. Once the backoff interval
  (starting at 1 s) became shorter than a real SNTP round trip, this could
  call `configTime()` again before the prior attempt ever completed,
  restarting it and starving synchronization indefinitely - a plausible
  explanation for "time sync requested" repeating for a long time.

  A first fix gated this on a caller-supplied `sntp_get_sync_status() ==
  SNTP_SYNC_STATUS_IN_PROGRESS` signal. A real-hardware re-test still showed
  several `time sync requested` lines before sync completed (recovering
  within 30 s regardless) - that status evidently can stay reset/idle for a
  while after `configTime()` is called, so it cannot serve as the sole
  guard on its own. The fix does not rely on it anymore: `DevMqttSmokeState`
  now tracks a single ConfigureTime attempt as in flight itself, entirely
  with its own state:

  - the moment `update()` issues `ConfigureTime`, it marks the attempt in
    flight and records a bounded, configurable deadline
    (`ntpAttemptTimeoutMs`, constructor parameter, default 15 s);
  - while in flight, `ConfigureTime` is never reissued, even past the
    ordinary backoff deadline, regardless of what any external status
    reports;
  - the attempt is cleared the moment `timeValid` becomes true (the real
    completion signal, driven by the caller's own sync-complete callback
    via `NtpClock::syncCompleted()`), letting the state machine continue
    into `WaitingForMqtt` immediately;
  - if `timeValid` never becomes true before the attempt's own timeout
    elapses, it is abandoned and exactly one fresh retry is allowed once the
    ordinary backoff deadline has also been reached - the timeout does not
    bypass that floor;
  - the timeout deadline reuses the same wrap-safe `deadlineReached()`
    comparison as the rest of the state machine; no sleep, busy wait, or
    wall-clock read was introduced.

  `NtpClock::syncInProgress()` (the `sntp_get_sync_status()` wrapper) is
  still used, but only for `hasValidTime()`'s own pre-existing settling gate
  (Phase 2D clock correction, below) - not to gate `ConfigureTime` retries.
  See `test_ntp_attempt_marks_in_flight_and_suppresses_reissue_until_timeout`,
  `test_ntp_sync_completion_clears_in_flight_and_continues_to_mqtt`,
  `test_ntp_attempt_timeout_allows_exactly_one_fresh_retry`,
  `test_ntp_attempt_timeout_is_wrap_safe`, and
  `test_ntp_attempt_in_flight_does_not_reissue_across_many_rapid_updates` in
  `test/test_dev_mqtt_state/test_main.cpp`. This does not by itself explain
  why SNTP took as long as it did to complete on the observed boot (that
  remains an unconfirmed network/environment hypothesis, not a diagnosed
  root cause) - it only removes a self-inflicted way the firmware could
  have kept restarting its own retry before it ever had a chance to
  succeed.
