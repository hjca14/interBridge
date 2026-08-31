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
| 3B.6 | Backend FCM sender: `telemetry_ingestion` invokes `push_sender`, which delivers a push notification via Firebase Cloud Messaging for `RING_DETECTED` (and related) Basic Ingest events | Backend repo | **Implemented and deployed in DEV** - the backend has accepted a real event end-to-end and recorded `Sent=1` for it |
| 3B.7 | Apply user/app notification preferences before the backend decides whether/how to notify | Backend repo | **Implemented and deployed in DEV** |
| **3B.8** | **Bench-only DEV physical ring simulator: a momentary button on the ESP32-C3 publishes a real `RING_DETECTED` event through the existing firmware/AWS IoT pipeline, for bench-testing 3B.6/3B.7 without a real Si3050/intercom line** | **`interBridge`** | **Implemented and compiled; five real-hardware boots so far. The concurrent-retry Wi-Fi coordinator defect is fixed and confirmed. The first four boots failed to associate (reason=201 on a WPA2 test hotspot, reason=2/202 on the home network, neither root-caused); the fifth found that disconnecting the button from GPIO20 let Wi-Fi associate cleanly to `Online`, and reconnecting it to GPIO20 dropped Wi-Fi again - the button has moved to GPIO4 for this reason. A retest confirming GPIO4 and isolating the remaining reason codes is still required. See `docs/dev-ring-simulator.md`'s five "Real bench observation" sections.** |
| 3B.9 | Android call/notification experience for an incoming ring | Mobile (Android) repo | **In progress** - the minimal slice (displaying a data-only FCM notification) is underway; the full call UI is not started |
| 3B.10 | iOS/APNs call/notification experience for an incoming ring | Mobile (iOS) repo | Not started (not in this repo) |

## Notes

- With 3B.6/3B.7 now deployed in DEV, 3B.8 is the remaining unvalidated
  link in the chain: a real button press on real hardware has not yet
  been exercised, so end-to-end delivery of a *physically triggered*
  notification has not been observed - only synthetic/backend-originated
  test events have been confirmed accepted (`Sent=1`). Do not describe
  3B.8 as validated, or the pipeline as proven end-to-end from a real
  button, until that hardware test actually happens - see
  `docs/dev-ring-simulator.md` > "Dependency on Phases 3B.6/3B.7" and >
  Honest status.
- Historical firmware phase numbers (3B.1, 3B.2, and all non-3B phases
  elsewhere in `CONTEXT.md`) are never renumbered by this document - it
  only adds the 3B.6-3B.10 continuation for cross-repo tracking.
- None of 3B.8's three real-hardware boot attempts reached Wi-Fi
  association, let alone the button/MQTT/event-delivery steps beyond it -
  do not treat 3B.8 as validated, or the 3B.6-3B.7 `Sent=1` confirmation
  as proof the full chain works from a real button, until a retest
  actually succeeds. The second retest found `esp32-c3-dev-mqtt` was never
  actually re-confirmed working on this exact bring-up path either - it
  had only connected successfully in an earlier, separate bench session,
  and shared the same coordinator bug the ring simulator hit. Do not
  assume a DEV environment still connects just because it did once before.
- The third retest confirmed the concurrent-retry fix worked (the
  driver-level "sta is connecting" error is gone) but association still
  fails with the same reason codes (2, 202) as before that fix - the
  concurrent retry was not the sole cause.
- A fourth retest against a dedicated WPA2 test hotspot ("Henrique's
  iPhone" Personal Hotspot) produced a *different* failure - reason=201
  (`no_ap_found`), meaning the ESP32 never saw that access point at all -
  while the home network still failed with reason=2/202 (an auth-stage
  rejection). Neither is root-caused, and this does not confirm or rule
  out a credential-pipeline bug, a range/band issue, or an AP-side
  rejection. Sanitized SSID/password byte-length + placeholder-match
  diagnostics and a controlled Wi-Fi scan (reported before every
  `WiFi.begin()`) have been added specifically to distinguish these on
  the next retest - see `docs/dev-ring-simulator.md` > "Wi-Fi config and
  scan diagnostics". Do not assume the credential or either AP is correct
  until that retest actually confirms it.
- A fifth test found a real, reproducible cause unrelated to credentials
  or either AP: with the ring-simulator button physically disconnected
  from GPIO20, Wi-Fi associated all the way to `Online`; reconnecting it
  to GPIO20 disconnected Wi-Fi again. The exact physical mechanism is
  still unconfirmed (alternate pin function, this mounting, this board
  sample, or RF/electrical interference are all still possible), and
  whether GPIO20 also explains the earlier reason=201/2/202 failures is
  not established - only that removing it let the firmware reach
  `Online` at least once. GPIO20 is no longer used for the ring
  simulator's button; it now uses GPIO4 (a deliberate, DEV-only overlap
  with the Si3050's DRX pin - safe only because this environment never
  touches the real Si3050). See `docs/dev-ring-simulator.md` > "Real
  bench observation: GPIO20 causes Wi-Fi to disconnect". A further
  retest on GPIO4 is required before treating Wi-Fi association as
  solved.
