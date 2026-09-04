# Phase 3C.1 BLE onboarding foundation

**Status: physically validated on a real ESP32-C3.** BLE advertisement (`InterBridge-6490` seen on Android/nRF Connect), Android app connection, and the Security 1 handshake up to its intentionally-blocked Wi-Fi-credentials step, plus the five-minute timeout correctly closing advertising and blocking further connection, have all been observed on real hardware. Actual Wi-Fi credential transfer (3C.3) and a handful of negative/retry checks remain unvalidated — see "Physical validation" below for exactly what is and is not confirmed.

## Compatibility baseline

All PlatformIO environments are pinned to `platform-espressif32` **6.12.0**. Its resolved Arduino framework is **Arduino-ESP32 2.0.17**, built on **ESP-IDF 4.4.7**. Arduino-ESP32 2.0.17 ships the official `WiFiProv` wrapper; that wrapper calls ESP-IDF Wi-Fi Provisioning Manager, the BLE scheme and Protocomm. Therefore a mixed-framework migration and a proprietary GATT protocol are unnecessary for this slice.

The upstream contracts consulted are PlatformIO's Espressif 32 6.12.0 release/package metadata, Arduino-ESP32 2.0.17 `WiFiProv` API/example, and ESP-IDF 4.4.7 Wi-Fi Provisioning Manager, Unified Provisioning and Protocomm Security 1 documentation. The isolated implementation selects `WIFI_PROV_SCHEME_BLE`, `WIFI_PROV_SCHEME_HANDLER_FREE_BTDM` and `WIFI_PROV_SECURITY_1`, which are the identifiers exposed by the pinned headers. The PoP is passed directly to the official wrapper and is never advertised or logged.

## Frozen Android contract for the parallel PR

| Item | Phase 3C.1 contract |
|---|---|
| Provisioning protocol | Espressif Unified Provisioning protocol v1, as implemented by ESP-IDF 4.4.7 / Arduino-ESP32 2.0.17 |
| Transport | Official Wi-Fi Provisioning Manager BLE scheme over Protocomm; no InterBridge-specific GATT service |
| Discovery name | `InterBridge-XXXX`, where `XXXX` is the uppercase final four hexadecimal characters of the stable, non-secret device identifier |
| Deduplication | During discovery, use the complete advertised device name. It is a short non-secret hint, not a fleet-wide identity guarantee; confirm the stable `device_id` through the authenticated product flow before permanent association |
| Security | Protocomm Security 1 with X25519 key agreement, AES-CTR protected payloads and proof of possession; Security 0/plaintext is forbidden |
| DEV PoP | Developer puts a non-production PoP in ignored `include/interbridge_ble_dev_secrets.h`; Android DEV receives the matching value through its own ignored local secret. It is never the 12-digit `setup_code` |
| Product PoP | Unique high-entropy value injected during manufacturing and delivered out-of-band to the authorized client. Exact storage/delivery remains a manufacturing/backend decision |
| Official endpoints | `proto-ver` (capability/version), `prov-session` (Security 1 handshake), and `prov-config` (Wi-Fi configuration/apply/status). Use Espressif protobuf contracts rather than constructing characteristics ad hoc |
| Observable states/errors | discovered/window open, authenticated request accepted, credentials accepted/rejected, disconnected, timeout, and sanitized internal failure |
| Retry | Disconnect before completion permits reconnect during the original five-minute window. A failed start can be retried. Timeout stops provisioning; an unprovisioned device reopens on boot and a provisioned device uses the future physical-button action |

The DEV firmware treats receipt of the first authenticated encrypted credential request as the observable evidence that both a BLE central and a secure Security 1 session exist. Arduino's wrapper does not expose separate public callbacks for “link connected” and “security handshake complete”; serial messages intentionally do not claim finer timing than its public event API provides.

This is a separate concern from advertising itself starting. `WiFiProv.beginProvision()` has no synchronous success/failure return, so the DEV firmware never treats the start *request* as evidence advertising is active; only the real `ARDUINO_EVENT_PROV_START` event does, via `Esp32BleProvisioning::notifyAdvertisingStarted()` and `BleOnboardingWindow` (`src/provisioning/ble_provisioning.h`). If that confirmation does not arrive within a short, explicit deadline, the attempt fails closed (sanitized "start not confirmed" log, provisioning manager released) rather than opening the five-minute window at all — see "Physical validation" below for the real-hardware gap this closes.

### Android constraints and Flutter status

- Android 12+ requires runtime `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT`; earlier Android versions can require location permission for BLE scanning.
- Scan by `InterBridge-`, tolerate duplicate callbacks, stop scanning before connecting, serialize GATT operations, and retry only while the firmware window is open.
- Espressif's Android provisioning implementation/protobuf definitions are the interoperability reference.
- Flutter plugin compatibility is **pending**. No plugin is approved until it demonstrates Unified Provisioning Security 1 and official endpoints without substituting custom GATT. Prefer a small native Android bridge rather than weakening the protocol if no maintained plugin qualifies.

## DEV build and local secret

```sh
cp include/interbridge_ble_dev_secrets.example.h include/interbridge_ble_dev_secrets.h
# edit only the ignored copy; never reuse a production PoP
pio run -e esp32-c3-dev-ble-provisioning
```

This composition contains only the BLE adapter and DEV entry point. It excludes the production root, AWS/MQTT composition, Si3050 code, and GPIO3/GPIO4 call simulators, preserving all accumulated behavior. At timeout the adapter calls ESP-IDF 4.4.7's public `wifi_prov_mgr_deinit()` API from `wifi_provisioning/manager.h`; that API stops an active provisioning service and releases manager/scheme resources, while the selected `WIFI_PROV_SCHEME_HANDLER_FREE_BTDM` handler releases Bluetooth controller memory. The timeout therefore ends the real provisioning service rather than merely changing local state. Logs contain only window/name/security mode, authenticated request observation, success/failure, disconnect and timeout—never PoP, SSID, Wi-Fi password, protobuf payload, or cryptographic material.

**Never set `CORE_DEBUG_LEVEL` or call `Serial.setDebugOutput(true)`/`esp_log_level_set()` with a verbose level in this (or any) environment.** A temporary `CORE_DEBUG_LEVEL=5` was used once during bench diagnosis (see "Physical validation" below) and, as a direct consequence, an upstream `WiFiProv.cpp` log line printed the DEV PoP to serial — a real security regression, now reverted. The exposed PoP was rotated locally and must be treated as compromised. `test_ble_provisioning` includes a native regression test (`test_isolated_ble_env_does_not_enable_verbose_core_debug`) that reads this repo's own `platformio.ini` and fails if `CORE_DEBUG_LEVEL` is ever set for this environment again. The only lines this environment may print are the sanitized `[BLE] ...` lines already in `src/dev/ble_provisioning_main.cpp` — never PoP, SSID, Wi-Fi password, protobuf payload, certificates, private keys, or raw provisioning internals.

The target alone selects Arduino-ESP32's official `huge_app.csv` partition
table for a 4 MB flash device. It retains NVS and provides a single enlarged
application slot without OTA, which is appropriate for this bench-only image.
It is not a production partitioning or OTA decision; every production and
pre-existing DEV environment retains its default table.

## Physical validation

**Real bench observation (first physical attempt):** after flashing `esp32-c3-dev-ble-provisioning`, the serial log eventually printed `onboarding window closed: timeout`, but nRF Connect never showed an `InterBridge-XXXX` device — the only nameless BLE entry visible did not disappear when the board was disconnected, so it was not the ESP32. The firmware had treated the `WiFiProv.beginProvision()` call itself as proof that advertising was active and had started its five-minute window and timer from that request, with no observation of the real start event. There was no evidence BLE advertising ever actually started. The firmware now requires the real `ARDUINO_EVENT_PROV_START` confirmation (with its own short, explicit timeout) before opening the window or claiming the service is active — see the previous section and `BleOnboardingWindow`'s doc comment in `src/provisioning/ble_provisioning.h`.

**Real bench observation (second physical attempt, after the fix above):** the serial log now reached `[BLE] provisioning manager initialized` (`ARDUINO_EVENT_PROV_INIT`) but then printed `[BLE] failure: onboarding service start not confirmed` — `ARDUINO_EVENT_PROV_START` never arrived within the confirmation deadline, and nRF Connect again found no `InterBridge-XXXX`, with no internal error visible at the default log level. A same-thread-ordering bug was found and fixed: the entry point called `BleOnboardingWindow::requestStart()` only *after* `startAdvertising()` returned, but `ARDUINO_EVENT_PROV_START` can be dispatched (on the system event task) essentially immediately once `WiFiProv.beginProvision()` is called, so a fast/synchronous confirmation could have been silently dropped as a no-op before the window was armed to receive it; `requestStart()` is now called first, with `close()` cleaning up if the subsequent `startAdvertising()` call is then locally rejected. A temporary `CORE_DEBUG_LEVEL=5` was also added at this point to capture ESP-IDF's own internal error for the next physical run — see the third observation below for how that instrumentation was used, what it found, and why it was then removed.

**Real bench observation (third physical attempt, success — with a security incident):** with the ordering fix and temporary verbose logging in place, the verbose capture showed the real root cause: `WiFiProv` correctly reported `Already Provisioned` and reconnected to a Wi-Fi network from a prior flash's persisted NVS state, instead of opening BLE at all — not a BLE/Protocomm defect. After erasing the bench board's flash/NVS and reflashing the isolated image, `ARDUINO_EVENT_PROV_INIT` and `ARDUINO_EVENT_PROV_START` both occurred, the ESP advertised as `InterBridge-6490`, Android/nRF Connect discovered it, the real Android app connected and advanced through the Security 1 onboarding flow to its intentionally-blocked Wi-Fi-credentials step (3C.3 is not implemented — see below), and after five minutes advertising stopped and a new connection was no longer possible. **However, the same verbose capture that found the NVS root cause also let a different upstream `WiFiProv.cpp` log line print the DEV PoP to serial.** That PoP is now treated as compromised and was rotated locally (the gitignored `include/interbridge_ble_dev_secrets.h` was never committed, so this was a bench-serial exposure only, not a repository leak). The fix: `CORE_DEBUG_LEVEL`, `Serial.setDebugOutput(true)`, and `esp_log_level_set()` were all removed from this environment (see "DEV build and local secret" above); normal operation now emits only this file's own sanitized `[BLE] ...` lines; and a native regression test (`test_isolated_ble_env_does_not_enable_verbose_core_debug` in `test_ble_provisioning`) fails the build if verbose core debug is ever reintroduced for this environment.

**A provisioned device intentionally does not reopen BLE.** The `Already Provisioned` behavior above is correct, expected `WiFiProv`/NVS behavior, not a bug: once Wi-Fi credentials are stored, the device reconnects instead of re-advertising. Bench retesting this isolated image on a board that was ever provisioned (by this image, the production firmware, or any other) therefore requires an explicit flash/NVS erase first — there is no in-band way to reopen the window yet. Production will not erase state on every boot: the intended design (not yet implemented — see Future Work) is a five-second physical button hold that explicitly clears *only* stored Wi-Fi provisioning state and reopens the pairing window, while preserving `device_id`/`setup_code` identity and any already-issued AWS IoT credentials.

**Confirmed on real hardware:** flash the isolated image; `ARDUINO_EVENT_PROV_INIT`/`ARDUINO_EVENT_PROV_START` fire and `[BLE] onboarding service active` is reached; advertising is observed on Android/nRF Connect with the name matching the serial log (`InterBridge-6490`); the app connects and establishes Security 1 with the correct PoP; the five-minute window closes advertising and a new connection is no longer possible afterward; a start that is never confirmed logs a sanitized "start not confirmed" failure rather than a misleading window timeout (observed during the second attempt above); this project's own sanitized `[BLE] ...` lines never contained the PoP or any other secret in any bench attempt, including the one where the *upstream, now-removed* verbose logging leaked it.

**Still unvalidated — do not assume these pass:**
- a physical re-run of the current, cleaned-up build (no verbose debug, reordered `requestStart()`) — expected to behave identically to the third attempt above since only diagnostic instrumentation was removed, but not yet physically reconfirmed;
- rejecting an incorrect PoP;
- disconnecting and reconnecting *within* the still-open five-minute window (the Android contract's retry allowance);
- **actual Wi-Fi credential transfer (3C.3) is not implemented and not validated** — this bench run only reached the point where the app would submit credentials, not `ARDUINO_EVENT_PROV_CRED_RECV`/`_SUCCESS`/`_FAIL` actually firing;
- anything downstream of credential receipt (Fleet Provisioning hand-off, `ProvisioningManager` integration) — this isolated image does not include `ProvisioningManager` at all.
