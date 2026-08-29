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
| 3B.6 | Backend FCM sender: subscribe to `RING_DETECTED` (and related) Basic Ingest events and deliver a push notification via Firebase Cloud Messaging | Backend repo | Not started (not in this repo) |
| 3B.7 | Apply user/app notification preferences before the backend decides whether/how to notify | Backend repo | Not started (not in this repo) |
| **3B.8** | **Bench-only DEV physical ring simulator: a momentary button on the ESP32-C3 publishes a real `RING_DETECTED` event through the existing firmware/AWS IoT pipeline, for bench-testing 3B.6/3B.7 without a real Si3050/intercom line** | **`interBridge`** | **Implemented and compiled; not yet validated on real hardware - see `docs/dev-ring-simulator.md`** |
| 3B.9 | Android call/notification experience for an incoming ring | Mobile (Android) repo | Not started (not in this repo) |
| 3B.10 | iOS/APNs call/notification experience for an incoming ring | Mobile (iOS) repo | Not started (not in this repo) |

## Notes

- 3B.8 (this repo's contribution) only proves the firmware→AWS IoT leg of
  the pipeline. It has no dependency on 3B.9/3B.10, but a phone
  notification will not actually be delivered end-to-end until 3B.6 and
  3B.7 exist on the backend - see `docs/dev-ring-simulator.md` >
  "Dependency on Phases 3B.6/3B.7".
- Historical firmware phase numbers (3B.1, 3B.2, and all non-3B phases
  elsewhere in `CONTEXT.md`) are never renumbered by this document - it
  only adds the 3B.6-3B.10 continuation for cross-repo tracking.
