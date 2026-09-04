# Phase 3C roadmap

| Phase | Scope | Status |
|---|---|---|
| 3C.1 | Real BLE onboarding foundation with official Unified Provisioning, isolated ESP32-C3 DEV image, Security 1 + PoP, and frozen mobile contract | **Physically validated on a real ESP32-C3**: advertisement, Android/nRF Connect discovery, Security 1 up to its Wi-Fi-credentials step, five-minute timeout. |
| 3C.2 | Android app: discovery, connection, Security 1 handshake up to the (then intentionally blocked) Wi-Fi-credentials step | **Merged** (app repository, not this one). |
| 3C.3 | Receive Wi-Fi credentials over the official `prov-config` apply/status flow (same isolated image) and connect | **Implemented; not yet physically validated** - requires a coordinated bench test with the 3C.2 app. No AWS IoT/Fleet Provisioning/certificate/claim flow - Wi-Fi connectivity only. |

See [`ble-onboarding.md`](ble-onboarding.md) for the compatibility decision, mobile contract, and physical validation status.
