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

## Physical validation still pending

No flash or radio test was performed. Phase 3C.1 remains open until:

1. flash the isolated image on the ESP32-C3;
2. observe advertising on Android;
3. confirm `InterBridge-XXXX`;
4. connect;
5. establish Security 1 with the correct PoP;
6. reject an incorrect PoP;
7. disconnect and reconnect;
8. confirm closure after five minutes;
9. confirm no secret appears in serial logs.
