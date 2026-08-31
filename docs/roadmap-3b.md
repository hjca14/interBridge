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
| 3B.6 | Backend FCM sender: `telemetry_ingestion` invokes `push_sender`, which delivers a push notification via Firebase Cloud Messaging for `RING_DETECTED` (and related) Basic Ingest events | Backend repo | **Implemented, deployed, and exercised in DEV** - initially with a synthetic event and subsequently by the controlled GPIO4 hardware run documented in 3B.8 |
| 3B.7 | Apply user/app notification preferences before the backend decides whether/how to notify | Backend repo | **Implemented and deployed in DEV** |
| **3B.8** | **Bench-only DEV physical ring simulator: a controlled active-high GPIO transition on the ESP32-C3 publishes a real `RING_DETECTED` event through the existing firmware/AWS IoT pipeline, for bench-testing 3B.6/3B.7 without a real Si3050/intercom line** | **`interBridge`** | **Done - validated end to end on real hardware from GPIO4 through AWS IoT/backend/FCM to the Android app; see `docs/dev-ring-simulator.md` > Validated state.** |
| 3B.9 | Android call/notification experience for an incoming ring | Mobile (Android) repo | **In progress** - the minimal slice (displaying a data-only FCM notification) is underway; the full call UI is not started |
| 3B.10 | iOS/APNs call/notification experience for an incoming ring | Mobile (iOS) repo | Not started (not in this repo) |

## Notes

- **3B.8 is complete.** A controlled LOW→HIGH transition on GPIO4 produced
  exactly one `RING_DETECTED` and one confirmed MQTT publish; the event
  traversed AWS IoT, `telemetry_ingestion`, `push_sender`, FCM, and appeared
  as a notification on Android. The preceding health report also made the
  device appear online in the app. This observes the minimal Android
  notification slice but does **not** complete 3B.9 or its full call UI.
- The successful stimulus was GPIO4 held LOW by an approximately 10 kΩ
  resistor to GND and pulsed momentarily to 3V3. It validates the active-high
  firmware path, not the Linker Button module, its wiring, held presses,
  repeated presses, or offline replay. Evaluating that module later is
  optional and separate from closing 3B.8.
- GPIO4 remains a provisional DEV-only overlap with `kSi3050PinPcmDrx`.
  It was safe because this isolated environment neither compiles nor
  initializes the Si3050 and no Si3050 was connected. It does not change
  production pinout or permit simultaneous button/DRX use; final assignment
  depends on the production board.
- The earlier GPIO20/Wi-Fi investigation remains useful history: an
  electrically mismatched button assembly correlated with loss of Wi-Fi,
  shared Wi-Fi coordination and diagnostic issues were fixed, and GPIO4 was
  selected provisionally. That history does not establish the Linker Button's
  electrical behavior or GPIO4 as a production choice.
- Historical firmware phase numbers are never renumbered by this document.
