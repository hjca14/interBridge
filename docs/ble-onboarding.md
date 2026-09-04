# Phase 3C.1 BLE onboarding foundation

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

This environment alone also sets `CORE_DEBUG_LEVEL=5` (Verbose) and the DEV entry point calls `Serial.setDebugOutput(true)` and `esp_log_level_set("*", ESP_LOG_VERBOSE)` right after the boot banner — diagnostic-only bench instrumentation, added after the second bench attempt (below) found no internal error visible at the default log level. No other environment sets this flag. The extra volume is internal ESP-IDF component/state logging (`wifi_prov_mgr`, `protocomm`, the Bluetooth controller, etc.) — PoP/SSID/password/protobuf/certificate material never passes through that logger in this flow by construction, but review a raw capture before sharing it, and do not carry `CORE_DEBUG_LEVEL=5` into any other environment.

The target alone selects Arduino-ESP32's official `huge_app.csv` partition
table for a 4 MB flash device. It retains NVS and provides a single enlarged
application slot without OTA, which is appropriate for this bench-only image.
It is not a production partitioning or OTA decision; every production and
pre-existing DEV environment retains its default table.

## Physical validation still pending

**Real bench observation (first physical attempt):** after flashing `esp32-c3-dev-ble-provisioning`, the serial log eventually printed `onboarding window closed: timeout`, but nRF Connect never showed an `InterBridge-XXXX` device — the only nameless BLE entry visible did not disappear when the board was disconnected, so it was not the ESP32. The firmware had treated the `WiFiProv.beginProvision()` call itself as proof that advertising was active and had started its five-minute window and timer from that request, with no observation of the real start event. There was no evidence BLE advertising ever actually started. The firmware now requires the real `ARDUINO_EVENT_PROV_START` confirmation (with its own short, explicit timeout) before opening the window or claiming the service is active — see the previous section and `BleOnboardingWindow`'s doc comment in `src/provisioning/ble_provisioning.h`.

**Real bench observation (second physical attempt, after the fix above):** the serial log now reached `[BLE] provisioning manager initialized` (`ARDUINO_EVENT_PROV_INIT`) but then printed `[BLE] failure: onboarding service start not confirmed` — `ARDUINO_EVENT_PROV_START` never arrived within the confirmation deadline, and nRF Connect again found no `InterBridge-XXXX`. The manager initializes but the BLE/Protocomm service itself does not come up, with no internal error visible at the default log level. Two things followed from this: (1) a same-thread-ordering bug was found and fixed — the entry point called `BleOnboardingWindow::requestStart()` only *after* `startAdvertising()` returned, but `ARDUINO_EVENT_PROV_START` can be dispatched (on the system event task) essentially immediately once `WiFiProv.beginProvision()` is called, so a fast/synchronous confirmation could have been silently dropped as a no-op before the window was armed to receive it; `requestStart()` is now called first, with `close()` cleaning up if the subsequent `startAdvertising()` call is then locally rejected. (2) `CORE_DEBUG_LEVEL=5` plus runtime `esp_log_level_set("*", ESP_LOG_VERBOSE)` were added, scoped to this environment alone, so the **next** physical run can actually capture ESP-IDF's own internal error/component name for why the service never starts — see "DEV build and local secret" above. Neither change is itself claimed to fix the underlying failure; both are prerequisites for observing it.

No flash or radio test has yet confirmed advertising actually starts. Phase 3C.1 remains open until:

1. flash the isolated image on the ESP32-C3;
2. **collect the verbose serial capture from `[BLE] provisioning manager initialized` through either `[BLE] onboarding service active` or `[BLE] failure: onboarding service start not confirmed`, and identify the specific internal ESP-IDF component/error (if any) logged in between — this is the immediate next step and the reason for the `CORE_DEBUG_LEVEL=5` addition above. Never record PoP, SSID, Wi-Fi password, protobuf payload, or cryptographic material, even at verbose level — review the capture before sharing it;**
3. confirm the serial log reaches `[BLE] onboarding service active` (i.e. `ARDUINO_EVENT_PROV_START` was actually observed, not just requested) before trusting anything on the Android/nRF Connect side;
4. observe advertising on Android/nRF Connect and confirm `InterBridge-XXXX` matches the name the serial log printed;
5. connect;
6. establish Security 1 with the correct PoP;
7. reject an incorrect PoP;
8. disconnect and reconnect;
9. confirm closure after five minutes from confirmed start (not from the request);
10. confirm a start that is never confirmed (e.g. radio/BLE stack failure) logs a sanitized "start not confirmed" failure instead of a misleading window timeout;
11. confirm no secret appears in serial logs, including the verbose capture from step 2.
