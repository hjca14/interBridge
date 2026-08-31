# Phase 3B roadmap

Phase 3B is the Si3050 hardware bring-up and ring-notification pipeline
effort. It spans this firmware repository (`hjca14/interBridge`) **and**
separate backend/mobile repositories - this document only tracks phase
numbers and one-line scope so the numbering stays consistent across
repos. It does not renumber or restate the detailed history of earlier
3B sub-phases already recorded in `CONTEXT.md`/`docs/si3050-*.md` - see
those files for the full record of 3B.1-3B.2.

| Phase | Scope | Where | Status |
|---|---|---|---|
| 3B.1 | Si3050 clock probe bench experiment (PCLK/FSYNC generation + measurement on isolated hardware) | `interBridge` | Done - see `docs/si3050-clock-probe.md` |
| 3B.2 | Real `Esp32PcmClock` PCM clock generation, integrated into normal firmware | `interBridge` | Done - see `docs/si3050-bringup.md` |
| 3B.3-3B.5 | Not tracked in this repository/document | - | Out of scope here |
| 3B.6 | Backend FCM sender: `telemetry_ingestion` invokes `push_sender`, which delivers a push notification via Firebase Cloud Messaging for `RING_DETECTED` (and related) Basic Ingest events | Backend repo | **Implemented and deployed in DEV** - the backend has accepted a real event end-to-end and recorded `Sent=1` for it (from a synthetic/backend-originated test event, not yet a real physical button press) |
| 3B.7 | Apply user/app notification preferences before the backend decides whether/how to notify | Backend repo | **Implemented and deployed in DEV** |
| **3B.8** | **Bench-only DEV physical ring simulator: a Linker Button module on the ESP32-C3 publishes a real `RING_DETECTED` event through the existing firmware/AWS IoT pipeline, for bench-testing 3B.6/3B.7 without a real Si3050/intercom line** | **`interBridge`** | **Implemented, compiled, and unit-tested. Not yet validated end to end on real hardware - see `docs/dev-ring-simulator.md` > Honest status for the full, consolidated record.** |
| 3B.9 | Android call/notification experience for an incoming ring | Mobile (Android) repo | **In progress** - the minimal slice (displaying a data-only FCM notification) is underway; the full call UI is not started |
| 3B.10 | iOS/APNs call/notification experience for an incoming ring | Mobile (iOS) repo | Not started (not in this repo) |

## Notes

- With 3B.6/3B.7 deployed in DEV, 3B.8 is the remaining unvalidated link
  in the chain: end-to-end delivery of a *physically triggered*
  notification has not been observed on real hardware. Do not describe
  3B.8 as validated, or the pipeline as proven end-to-end from a real
  button, until a hardware retest actually confirms it - see
  `docs/dev-ring-simulator.md` > "Dependency on Phases 3B.6/3B.7" and >
  Honest status.
- Historical firmware phase numbers (3B.1, 3B.2, and all non-3B phases
  elsewhere in `CONTEXT.md`) are never renumbered by this document - it
  only adds the 3B.6-3B.10 continuation for cross-repo tracking.
- **3B.8 bench testing summary** (full chronology in
  `docs/dev-ring-simulator.md` > Bench test history): several real
  hardware sessions found and fixed a genuine shared Wi-Fi coordination
  defect in `DevMqttSmokeState` (also affecting `esp32-c3-dev-mqtt`) and
  a diagnostic-only `uint32_t` underflow bug, and added sanitized
  credential/scan diagnostics after Wi-Fi association failed with
  different disconnect reasons on two different networks. One session
  reached full local connectivity (`Online`) with the ring-simulator
  button disconnected from the board; reconnecting it (to what was then
  GPIO20) dropped Wi-Fi again. That assembly was later found to be
  electrically mismatched with the firmware's assumptions (a Linker
  Button module, active-high, needs `VCC`/`GND`/`SIG` - not a dry,
  active-low contact) - so this does **not** establish GPIO20, the
  credential, or either access point as the cause of anything. The
  button now uses GPIO4 with the module's correct electrical interface;
  **a hardware retest with that correct wiring has not yet happened**,
  and the underlying Wi-Fi association failures remain unexplained.