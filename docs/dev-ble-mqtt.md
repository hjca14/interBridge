# DEV BLE onboarding + connectivity composition (Phase 3C.4)

## Objective

Phase 3C.4 adds one more bench-only DEV firmware environment,
`esp32-c3-dev-ble-mqtt`, that chains two things already physically
validated **separately** into a single bench image: the official BLE
onboarding session (Phase 3C.1/3C.3 - Security 1, PoP, Wi-Fi credential
receipt, invalid-credential recovery) and the NTP → AWS IoT/MQTT →
health-report cascade (`esp32-c3-dev-mqtt`), plus the GPIO4/GPIO3
call-session simulator (`esp32-c3-dev-ring-simulator`). After a user
provisions Wi-Fi through the validated BLE flow on a real Android or
iPhone, the same ESP32-C3 DEV image should continue automatically into
NTP, MQTT, and a health report, and appear online in the app - without a
reflash between onboarding and connectivity.

**Implemented, not yet physically validated.** Every individual piece
this composition reuses is already validated on real hardware on its own
(see `docs/ble-onboarding.md` and `docs/dev-ring-simulator.md`); the new,
unvalidated thing here is the *hand-off* between them on one board in one
boot. See "Bench validation checklist" below.

## What this is not

Same boundary as every prior DEV phase, unchanged:

- **Not production.** This remains a bench-only DEV composition, gated
  behind its own PlatformIO environment - never linked into `esp32-c3`
  (production), and none of the other DEV/Si3050-probe environments are
  linked into it.
- **A successful Wi-Fi connection is Wi-Fi connectivity only.** It does
  not mean the device is claimed, registered with any backend, or that
  AWS IoT Fleet Provisioning ran - this composition uses the same
  manually-provisioned DEV MQTT device/credential material
  `esp32-c3-dev-mqtt`/`esp32-c3-dev-ring-simulator` already use (see
  `docs/mqtt-dev-smoke-test.md`), reused **only** for this bench
  environment - never presented as a production device identity or
  manufacturing flow.
- **GPIO4/GPIO3 remain temporary DEV call-session substitutes**, exactly
  as in `esp32-c3-dev-ring-simulator` - see
  `docs/dev-ring-simulator.md` > "Scope and safety". Neither the Si3050,
  the real Linker Button, nor the analog intercom line is exercised or
  validated here.
- **Not implemented here:** AWS IoT Fleet Provisioning, certificates
  issued at runtime, claim/ownership, production `setup_code`, OTA, and
  the eventual production physical reset (a 5-second button hold that
  clears only stored Wi-Fi state and reopens the pairing window, with a
  fast-blinking LED, preserving `device_id`/`setup_code`/already-issued
  AWS credentials - see `docs/ble-onboarding.md`'s "Physical validation"
  section, "A provisioned device intentionally does not reopen BLE").
  Erasing flash/NVS remains a bench-only re-test mechanism, never a
  product flow, in this composition too.

## What is reused, unmodified

Nothing in this composition is a new state machine, credential store, MQTT
client, or provisioning implementation - `src/dev/ble_mqtt_main.cpp` is
composition/wiring only, in the same spirit as
`src/dev/dev_command_environment.h`:

- **BLE onboarding**: `Esp32BleProvisioning`, `BleOnboardingWindow`,
  `buildBleAdvertisementInfo()` (`src/provisioning/ble_provisioning.h/.cpp`)
  - the exact 3C.1/3C.3 classes, including the
  `wifi_prov_mgr_reset_sm_state_on_failure()` invalid-credential recovery
  call. `INTERBRIDGE_DEV_BLE_PROVISIONING` is defined for this
  environment too (see `platformio.ini`) - the same compile-time gate
  that activates `Esp32BleProvisioning`'s real WiFiProv-backed
  implementation, deliberately reused rather than duplicated behind a
  second flag.
- **Connectivity cascade**: `DevMqttSmokeState`
  (`src/dev/mqtt_smoke_state.h/.cpp`), completely unmodified - see
  "Wi-Fi hand-off" below for how this composition drives it without
  compile-time Wi-Fi credentials.
- **AWS IoT/MQTT**: `Esp32AwsIotTransport`, `MqttTopics`,
  `DeviceCredentialStore`, `HealthReporter`, `DevCommandEnvironment` (the
  same shared `InMemoryDedupCache`/`DisabledHardware`/`Intercom`/
  `DisabledSystemControl`/`CommandHandler`/`RemoteCommandProcessor`
  composition `esp32-c3-dev-mqtt`/`esp32-c3-dev-ring-simulator` already
  use) - all unmodified. The manually-provisioned DEV device/credential
  material itself is the same one those two environments already use, now
  read from this environment's own combined header - see "Local
  credentials and build" below.
- **Call-session simulator**: `DevRingButtonController`,
  `DevRingEventCoordinator`, `MemoryEventOutbox`, `publishPendingEvents()`
  (`src/dev/dev_ring_button.*`, `src/dev/dev_ring_event.*`) on the same
  GPIO4 (start)/GPIO3 (end) pins as `esp32-c3-dev-ring-simulator` - all
  unmodified. Button handling runs every loop iteration regardless of
  BLE/connectivity state, exactly as in that environment.

The one genuinely new piece is the hand-off boundary itself -
`BleMqttHandoffGate` (`src/dev/ble_mqtt_handoff.h`, natively unit-tested in
`test/test_ble_mqtt_handoff`) - see below.

## Wi-Fi hand-off: who owns `WiFi.begin()`

`DevMqttSmokeState` (unmodified) issues a `ConnectWifi` action whenever
`wifiConnected` is false and no attempt is currently in flight; every
other DEV environment responds by calling `WiFi.begin(SSID, PASSWORD)`
with compile-time credentials. This composition has no compile-time
Wi-Fi credentials at all - the whole point of BLE onboarding is that the
user supplies them at runtime - so it cannot do the same thing, and must
not let its own `WiFi.begin()` call race the official
`wifi_provisioning` manager's own `esp_wifi_connect()` call for the
credential it just received (or the one it may be reconnecting with
directly, in the "already provisioned" case below).

`BleMqttHandoffGate` is a small, one-shot latch: `bleOwnsWifi()` starts
`true` and becomes `false` exactly once, via `markProvisioningEnded()`.
While `true`, `ble_mqtt_main.cpp`'s `ConnectWifi` handler does nothing
(no `WiFi.begin()`) - `DevMqttSmokeState`'s own provisional in-flight
timeout simply lapses harmlessly and it retries later with backoff,
never advancing past `WaitingForWifi` on its own. Once `false`, the
`ConnectWifi` handler calls the **no-argument** `WiFi.begin()` overload,
which reconnects using whatever STA configuration the Wi-Fi driver
already has stored in NVS (originally provided over BLE, or from an even
earlier boot) - never a credential this firmware itself holds.

Every other `DevSmokeAction` (`ResolveDns`, `ConfigureTime`,
`ConnectMqtt`, `RecoverWifi`) is left ungated: those can only ever fire
once `DevMqttSmokeState` has already observed a genuine
`wifiConnected == true`, at which point the interface is already
associated (by BLE or otherwise) and using it for DNS/NTP/MQTT is not an
association operation that could race the provisioning manager.
`RecoverWifi` in particular is deliberately left ungated even though it
calls `WiFi.disconnect()`: gating it risks `DevMqttSmokeState`'s
`awaitingWifiRecoveryDisconnect_` latch waiting forever for a disconnect
this file would then never actually issue, and in practice it can only
fire long after BLE onboarding has already concluded.

`markProvisioningEnded()` is called from two places, both safe to call
more than once:

1. `ARDUINO_EVENT_PROV_END` - the authoritative signal, covering a
   successful credential application, a rejected/never-confirmed BLE
   session, or the manager concluding on its own.
2. This environment's local `BleOnboardingWindow` bookkeeping
   (`StartNotConfirmed`/`WindowTimedOut`) - a defensive fallback in case
   the real event is ever missed, and the only way this environment
   currently has to make forward progress in the "already provisioned"
   case below, where `ARDUINO_EVENT_PROV_START` may never fire at all.

## The "already provisioned" fork (unconfirmed on real hardware)

`docs/ble-onboarding.md`'s third bench observation records that
`WiFiProv.beginProvision()` (called unconditionally by this
composition's `setup()`, exactly like the isolated 3C.1 target) detects
an already-provisioned device and reconnects directly from stored NVS
credentials **instead of ever opening BLE** - in that observation,
`ARDUINO_EVENT_PROV_START` never fired. Whether `ARDUINO_EVENT_PROV_END`
fires promptly in that same case, on this exact pinned Arduino-ESP32
2.0.17/ESP-IDF 4.4.7 combination, has not been confirmed.

To stay safe under that uncertainty, `ble_mqtt_main.cpp` does **not**
call `provisioning.stopAdvertising()` (which calls
`wifi_prov_mgr_deinit()`) merely because the local confirmation window's
`StartNotConfirmed` event fires - only a genuine `WindowTimedOut` (a BLE
session that was confirmed active and then genuinely ran out its
five-minute window) does that. `StartNotConfirmed` only releases the
`BleMqttHandoffGate` hand-off, so the ordinary connectivity cascade can
proceed (or safely retry `WiFi.begin()` later) regardless of which
interpretation turns out to be correct on real hardware. This exact fork
is item 6 of the checklist below.

## Local credentials and build

This environment has **no compile-time Wi-Fi credentials of any kind** -
neither `INTERBRIDGE_DEV_WIFI_SSID` nor `INTERBRIDGE_DEV_WIFI_PASSWORD`
may ever be defined or referenced here (`scripts/check_repo_safety.py`
enforces this in CI); Wi-Fi comes exclusively from the BLE session and
whatever the ESP-IDF Wi-Fi driver then persists in NVS - see "Wi-Fi
hand-off" above. It depends on exactly one ignored local header,
`include/interbridge_dev_ble_mqtt_secrets.h`, carrying only the AWS
IoT/MQTT identity and the BLE DEV PoP - never the two older, per-purpose
headers (`interbridge_dev_secrets.h`, `interbridge_ble_dev_secrets.h`)
the isolated `esp32-c3-dev-mqtt`/`esp32-c3-dev-ble-provisioning`
environments use.

1. On the Mac, keep exactly five files in one directory **outside** this
   repository, e.g. `~/interbridge-dev-credentials/`:
   - `endpoint.txt` - the AWS IoT ATS endpoint, one line.
   - `AmazonRootCA1.pem`
   - `device-certificate.pem.crt`
   - `private.pem.key`
   - `pop.txt` - the local DEV PoP, one line, exactly 64 lowercase
     hexadecimal characters. Generate one with
     `openssl rand -hex 32 > ~/interbridge-dev-credentials/pop.txt`; treat
     it exactly like a password (see `docs/ble-onboarding.md`'s DEV PoP
     exposure incident for why).
2. Run one of the two equivalent generators - both read the same five
   files, apply the same validation, and write the exact same header;
   pick whichever is available on the Mac in use. `pwsh` (PowerShell) is
   not guaranteed to be installed/on `PATH` even when the cask is present
   (observed on an Intel Mac bench) - the Bash generator needs nothing
   beyond what macOS ships:

   **Bash (macOS, no installable dependency):**

   ```sh
   bash ./scripts/generate_dev_ble_mqtt_secrets_header.sh \
     ~/interbridge-dev-credentials \
     ib-<32-lowercase-hex>
   ```

   **PowerShell (where `pwsh` is available):**

   ```sh
   pwsh ./scripts/generate_dev_ble_mqtt_secrets_header.ps1 \
     -CredentialsDirectory ~/interbridge-dev-credentials \
     -DeviceId ib-<32-lowercase-hex>
   ```

   Either one reads only those five files by fixed name, validates the
   endpoint format, that `pop.txt` is exactly 64 lowercase hex characters,
   that nothing is empty or still the example placeholder, that the
   destination is Git-ignored, and that the credentials directory is
   outside the repository - then writes the six escaped one-line C++
   macros to `include/interbridge_dev_ble_mqtt_secrets.h` (generated fresh
   each run; never edit it by hand). Neither accepts certificate, private
   key, PoP, SSID, or password material as a command-line argument, and
   neither ever prints a value, length, hash, or prefix derived from the
   certificate, private key, or PoP - only confirms success and echoes
   back the (non-secret) `device_id` you passed in.
3. Run `pio run -e esp32-c3-dev-ble-mqtt` and flash that environment
   explicitly. Selecting it without the generated header fails at
   preprocessing with a clear error, exactly like the other DEV
   environments' own local headers.

Reuse the same DEV device/credential material `esp32-c3-dev-mqtt`/
`esp32-c3-dev-ring-simulator` already use if you have it - this is a
bench convenience only, never a production identity or manufacturing
flow (see "What this is not" above).

## First physical attempt (2026-09-05): DNS precondition bug

The first bench attempt of this composition reached BLE Security 1,
received Wi-Fi credentials, and observed `wifi event=got_ip` - but then
stayed at `DNS: pending`, triggered `wifi recovery requested`, and never
reached NTP/MQTT. The configured AWS IoT endpoint was independently
confirmed resolvable with `nslookup` from outside the ESP, ruling out the
endpoint, certificate, PoP, and BLE stack.

Root cause: both `DevSmokeAction::ResolveDns` and the network preflight
immediately before `DevSmokeAction::ConnectMqtt` required
`WiFi.dnsIP() != IPAddress()` before even attempting
`WiFi.hostByName(...)`. `WiFi.dnsIP()` is a separate Arduino-ESP32 wrapper
value that is not reliably populated even once the STA interface is fully
associated with a valid local IP - gating the *attempt* on it (rather than
just gating on it being informative) meant `hostByName()` was never
called at all on this run, even though DNS itself worked.

Fix: the precondition for attempting `hostByName()` is now a connected STA
interface with a valid local IP only (`WiFi.status() == WL_CONNECTED &&
WiFi.localIP() != IPAddress()`), extracted as the pure, natively-tested
`isReadyToAttemptDnsResolution()` (`src/dev/dev_dns_readiness.h/.cpp`,
`test/test_dev_dns_readiness`) and applied identically in both call sites.
A genuine DNS failure (network ready but `hostByName()` itself returns
failure) still reports `DNS: pending`/`network preflight dns=failed` and
still drives `DevMqttSmokeState`'s existing recovery path unchanged - this
fix only changes when the lookup is attempted, never whether a real
failure is treated as one.

**Not yet physically re-validated** - this fix has not yet been reflashed
and confirmed on the bench. See the checklist below.

## Bench validation checklist

Short by design - every individual capability below is already validated
elsewhere; this list only covers the new hand-off.

1. Erase only this board's DEV Wi-Fi state when a scenario needs to be
   repeated (a fresh BLE window on an already-provisioned board).
2. Provision via the real Android or iPhone app.
3. Confirm Wi-Fi connects, NTP synchronizes, MQTT connects, and a health
   report publishes - all without a reboot or reflash after onboarding.
4. Confirm the device shows online in the app.
5. Trigger GPIO4 then GPIO3 and confirm `RING_DETECTED`/`RING_ENDED`
   through the existing, already-validated chain.
6. Reboot **without** erasing Wi-Fi state and confirm MQTT/health
   reconnect on their own, with no BLE advertising reopening - this is
   also the real-hardware answer to the "already provisioned" fork above.
