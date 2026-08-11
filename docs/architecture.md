# InterBridge Firmware — Architecture

This document describes the current architecture of the InterBridge
firmware. For project status, open decisions and change history, see
[`CONTEXT.md`](../CONTEXT.md) — this file describes *how the pieces fit
together*, CONTEXT.md tracks *what is true right now*.

## Goals

- Keep business logic (state machine, intercom logic, event flow)
  independent of Arduino/ESP32 APIs so it can be unit tested on a host
  machine, ahead of final hardware being available.
- Isolate every hardware-specific or protocol-specific unknown behind an
  interface, so those decisions can be made later without reworking the
  rest of the firmware.
- Keep `main.cpp` a thin composition root: it wires modules together and
  contains no business logic itself.

## Module map

```text
src/
├── main.cpp          Composition root: setup()/loop(), wires modules together.
│
├── core/              Cross-cutting, hardware-independent building blocks.
│   ├── events.h/.cpp       Strongly-typed Event/EventType.
│   ├── state_machine.h/.cpp  Call-flow state machine.
│   ├── logger.h/.cpp       Structured logging with a swappable sink.
│   └── version.h           FIRMWARE_VERSION.
│
├── hardware/          Hardware Abstraction Layer (HAL).
│   └── gpio.h/.cpp     IHardwareIO interface + Esp32GpioHardware (stub).
│
├── intercom/           Intercom business logic (electrically agnostic).
│   ├── line_detector.h/.cpp  Off-hook/on-hook edge detection over IHardwareIO.
│   └── intercom.h/.cpp       Intercom facade used by main.cpp.
│
├── audio/              Audio abstraction (not implemented).
│   └── audio.h/.cpp     IAudioIO interface + NullAudioIO placeholder.
│
└── network/            Network abstraction, split by concern.
    ├── wifi.h/.cpp       Wi-Fi station link (ESP32 WiFi.h wrapper).
    └── protocol.h/.cpp   ICommunicationProtocol interface + NullProtocol placeholder.
```

`include/` and `test/` are reserved by the PlatformIO project layout;
`include/` is currently unused (see `include/README.md`), `test/` holds
the native unit tests described below.

## Layering / dependency direction

```text
        main.cpp  (composition root, Arduino-only)
            │
            ▼
   ┌────────────────────────────────────────┐
   │   core/   intercom/   audio/   network/  │   business logic
   └────────────────────────────────────────┘
            │            depends on interfaces only
            ▼
   ┌────────────────────────────────────────┐
   │              hardware/ (HAL)             │
   └────────────────────────────────────────┘
            │
            ▼
        ESP32-C3 / Arduino APIs
```

`core/`, `intercom/` and `audio/` never include `Arduino.h` or reference
GPIO numbers directly — they depend on `IHardwareIO` (an interface), not
on `Esp32GpioHardware` (its concrete implementation). This is what makes
`test/test_line_detector` possible without any real hardware: the test
injects a `MockHardware` that implements `IHardwareIO`.

`network/wifi.*` is the one exception that *does* depend on Arduino
(`WiFi.h`) directly, because Wi-Fi itself is a confirmed requirement, not
an unknown. `network/protocol.*`, by contrast, stays hardware- and
transport-agnostic because the actual application protocol has not been
chosen yet.

## Event flow (current)

```text
IHardwareIO.readLineState()
        │
        ▼
   LineDetector.update()  ──emits──▶  Event{OffHook | OnHook}
        │
        ▼
   Intercom.update()  (currently pass-through)
        │
        ▼
   main.cpp: updateIntercom()  ──▶  Logger::event(...)
        │
        ▼
   StateMachine.handleEvent(event)  ──▶  Logger::stateTransition(...) via callback
```

`WifiManager.update()` follows the same shape for `WifiConnected` /
`WifiDisconnected`, feeding into the same `StateMachine.handleEvent()`
call from `main.cpp`.

`RingDetected`, `DoorOpenRequested`, `CallStarted` and `CallEnded` are
defined in `core/events.h` but nothing produces or consumes them yet —
see [`CONTEXT.md` > Future Work](../CONTEXT.md#future-work).

## State machine

```text
   BOOT
     │  finishBoot()  (explicit call from main.cpp, not an event)
     ▼
   IDLE
     │  RingDetected
     ▼
  RINGING ──OnHook──▶ IDLE   (caller hung up before being answered)
     │
     │  OffHook
     ▼
  IN_CALL
     │  OnHook
     ▼
   IDLE

   (any state) ──reportFault()──▶ ERROR   (no recovery path yet)
```

This is intentionally the minimal set needed to represent the basic call
cycle. `CONNECTING_WIFI`, `READY`, `OPENING_DOOR` and `RECOVERING` are
anticipated future states (see CONTEXT.md) and are deliberately **not**
implemented yet, to avoid designing transitions around behavior that
hasn't been characterized on real hardware.

## Testing strategy

Two PlatformIO environments exist:

- `esp32-c3` — the real firmware, `framework = arduino`.
- `native` — compiles the hardware-independent subset of `src/` together
  with Unity tests under `test/`, using the host's own C++ compiler. This
  is what lets `core/`, `intercom/` and `network/protocol.*` be tested
  without any ESP32 or intercom hardware.

`main.cpp` and `network/wifi.cpp` are excluded from the `native` build
(via `build_src_filter` in `platformio.ini`) because they include
Arduino-only headers.

See `CONTEXT.md > Tests` for what is currently covered and what still
needs on-hardware validation.

## Why these boundaries specifically

- **`IHardwareIO` as a single narrow interface** rather than exposing
  GPIO-shaped methods per feature: the electrical design isn't known, so
  the interface models *intent* (`readLineState`, `setDoorOutput`) rather
  than mechanism. Expect this interface to grow, not to be redesigned.
- **`WifiManager` vs. `ICommunicationProtocol` are separate modules**
  because "how the ESP32 gets on the network" and "what it says to the
  backend once connected" are independent decisions — conflating them
  would force a protocol choice into the Wi-Fi layer.
- **`NullAudioIO` / `NullProtocol` instead of `nullptr`** so `main.cpp`
  can hold a concrete object today without a runtime null check, and swap
  it for a real implementation later without changing call sites.
