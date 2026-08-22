# InterBridge Communication Protocol

**Status:** Draft v1.3 — BLE-first onboarding update  
**Protocol version:** `1`  
**Target firmware:** InterBridge Firmware `0.1.x`  
**Primary target:** ESP32-C3  
**Cloud device plane:** AWS IoT Core  
**Primary transport:** MQTT over TLS with mutual X.509 authentication

**v1.3 note:** the onboarding architecture changed. **Nearby BLE
discovery is now the primary onboarding UX** (section 7.1); QR scanning
and manual code entry are fallback identity-resolution methods only
(section 7.3), never a separate provisioning path. The human-facing
product code is renamed from `claim_code` to **`setup_code`** (12
decimal digits, permanent, not a secret/credential - section 4.2) to
stop conflating it with the temporary application claim session and the
AWS Fleet Provisioning temporary claim (see section 4's terminology
note). A dedicated provisioning state machine (section 7.5), a 5-minute
provisioning window with in-window retry (section 7.6), a BLE
advertisement model (section 7.4), and a semantic status-indication
interface (section 7.7) are new in this revision. See CONTEXT.md's
change log for the full list.

**v1.2 note (prior revision):** reconciled this document with the
firmware implementation - command timestamp format, the full error code
set with per-code origin, and the closed `intercom_state` vocabulary.

Neither revision implements AWS IoT Core, real MQTT/TLS, NTP, real BLE
(ESP-IDF Unified Provisioning), real NVS, Lambda, API Gateway, or
Cognito - see `CONTEXT.md` for exactly what remains stubbed ahead of
Phase 1.

---

## 1. Purpose and Scope

This document is the source of truth for communication between the InterBridge firmware and the cloud device plane.

It defines device identity, BLE provisioning, AWS IoT Fleet Provisioning, X.509 credentials, MQTT behavior, application topics, Device Shadow usage, Basic Ingest, commands, acknowledgements, reconnect/retry, persistent configuration, config/reset button behavior, AWS IoT Jobs, OTA, signing, rollback, and the security baseline.

Audio is explicitly outside this control-plane protocol and must use a separate low-latency transport.

---

## 2. Architecture

```text
                         InterApp
                            │
                    HTTPS / application APIs
                            │
                            ▼
                    AWS application backend
                  Cognito / API / Lambda etc.
                            │
               ┌────────────┴─────────────┐
               │                          │
               ▼                          ▼
       Application storage          AWS IoT Core
                                          │
                                    MQTT over TLS
                                    X.509 / mTLS
                                          │
                                          ▼
                                   InterBridge
                                     ESP32-C3
```

Onboarding (first-time and re-opened) is BLE-first:

```text
InterApp
   │
   │ BLE + secure provisioning (Protocomm)
   ▼
InterBridge
```

The app reaches this BLE session in one of three ways - nearby discovery
(primary), QR scan (fallback), or manual `setup_code` entry (fallback) -
see section 7 for the full flow. All three converge on the same session
and the same device-side provisioning coordinator; there is no separate
"QR onboarding" implementation.

The firmware must keep cloud integration behind abstractions so intercom, GPIO, state-machine and audio logic do not directly depend on AWS calls.

---

## 3. Core Decisions

- AWS IoT Core is the device gateway.
- MQTT over TLS is the device control-plane transport.
- Each device authenticates with its own X.509 certificate/private key using mTLS.
- The permanent key pair is generated on-device; the private key never leaves the InterBridge.
- AWS IoT Thing name and MQTT ClientId use the stable `device_id`.
- **Onboarding is BLE-first**: the primary flow is nearby BLE discovery -
  the app finds a compatible, advertising InterBridge, the user confirms
  it's the right physical unit, and provisioning proceeds entirely over
  BLE. QR scanning and manual `setup_code` entry are **fallback**
  identity-resolution methods only, for when nearby discovery isn't
  practical (e.g. multiple units nearby, or the app can't distinguish
  them) - they resolve to the same physical device and converge on the
  exact same BLE session/provisioning coordinator. Neither is required
  for normal onboarding.
- BLE provisioning uses ESP-IDF Unified Provisioning / Protocomm, with
  the strongest security mode the pinned ESP-IDF version supports
  cleanly (Security2 preferred, Security1 an acceptable fallback);
  plaintext/unsecured provisioning is never used.
- BLE provisioning uses a unique PoP per physical device.
- `setup_code` (12 human-readable decimal digits) is the product's
  human-facing onboarding identifier, used only by the QR/manual
  fallback paths - see section 4.2. It is distinct from the temporary
  application claim session and the AWS Fleet Provisioning temporary
  claim (section 4).
- An unprovisioned device automatically advertises for onboarding at
  boot; a provisioned device does not advertise unless the physical
  config/reset button is held ~3 seconds. Either way, the provisioning
  window is 5 minutes - see section 8.
- Permanent device credentials are issued with AWS IoT Fleet Provisioning by Trusted User.
- Persistent device state/config uses a named AWS IoT Device Shadow.
- Cloud-side online/offline uses AWS IoT lifecycle/connectivity events.
- Backend-only event/telemetry ingestion should use AWS IoT Basic Ingest.
- Command responses also use Basic Ingest and are correlated by the backend.
- Commands use normal MQTT broker topics.
- OTA uses AWS IoT Jobs + S3 + signed firmware + ESP32 rollback.
- Audio transport is separate from MQTT.
- A physical config/reset button is required; GPIO remains TBD.

---

## 4. Device Identity

Each InterBridge has a stable technical identity plus a human-facing
onboarding identifier used only by the fallback claim paths (section 7.3):

```text
device_id     technical identity - AWS ThingName, MQTT ClientId
hardware_version
setup_code    human-facing onboarding identifier (QR/manual fallback only)
```

**Terminology note (do not conflate these three):**

```text
setup_code                              human-facing, low-entropy, printed on the device/packaging - section 4.2
temporary application claim session      short-lived, backend-authenticated, established after BLE discovery - section 6.1
AWS Fleet Provisioning temporary claim    AWS-issued, used only for the CSR flow - section 6.2
```

### 4.1 device_id

Required production format:

```text
device_id = ib-<128-bit lowercase hexadecimal identifier>
```

Example:

```text
ib-7f3a91c2d84e4fa9b621b88658fdca77
```

The identifier must be generated from at least 128 bits of cryptographically secure
randomness. It is not a secret, but it must be globally unique and impractical to
enumerate at fleet scale. The user is never expected to type or read this value -
see section 7 for how the app actually finds a physical device.

### 4.2 setup_code

```text
format:  12 decimal digits
example: 482719362051
display: grouped in 4s, e.g. "4827 1936 2051"
```

`setup_code` is a manufacturing/onboarding identifier, generated once and
never regenerated (see `provisioning/device_identity.h`'s
`DeviceIdentityProvider`, which loads-or-generates it exactly like
`device_id`). It is explicitly **not**:

```text
a private key
an AWS credential
an MQTT identity
a permanent authorization token
```

and does not need 128 bits of cryptographic entropy - its job is to let a
human (or a QR code) unambiguously identify *which physical unit* is
being set up; the actual security boundary for onboarding is the BLE
session (section 7) and the backend-authenticated claim session (section
6.1), not `setup_code` itself. It should still not be casually logged or
exposed outside the onboarding flow, as a matter of good practice, but it
is not treated as a secret requiring the same handling as a Wi-Fi
password or a private key.

`setup_code` is used **only** by the fallback identity-resolution paths
(QR, manual entry - section 7.3). The primary onboarding flow (nearby BLE
discovery - section 7.1) never requires the user to see or enter it.

### 4.3 QR code format (fallback only)

The QR code encodes a single URI in the canonical form:

```text
interbridge://claim?v=1&device_id=ib-<32 lowercase hex chars>&setup_code=<12 digits>
```

Example:

```text
interbridge://claim?v=1&device_id=ib-0123456789abcdef0123456789abcdef&setup_code=482719362051
```

Validation rules the app (and any backend that parses a scanned code)
must enforce:

```text
scheme                must be exactly "interbridge"
host                  must be exactly "claim"
v                     must be exactly "1"
device_id             must match ^ib-[0-9a-f]{32}$
setup_code             must match ^[0-9]{12}$
duplicate query params  must be rejected (e.g. two "device_id" values)
parameter values         must be percent-decoded before validation
```

Once the app has resolved a physical device via this QR (or via manual
entry of the same 12-digit code), it opens the exact same BLE
provisioning session used by the primary nearby-discovery flow - see
section 7.3. QR/manual are identity-resolution shortcuts only; there is
no separate provisioning implementation behind them.

The firmware does not scan or parse this QR code - it only appears on
the physical device/packaging for the app to read. No QR
scanner/parser is implemented in this repository, and none is needed for
Phase 1.

It must never contain permanent private keys, permanent AWS IoT credentials, or backend administrative secrets.

AWS identity:

```text
ThingName = device_id
ClientId  = device_id
```

AWS IoT policies must enforce this relationship.

---

## 5. Device Authentication

Production authentication:

```text
X.509 client certificate
+
device private key
+
TLS server certificate validation
=
mutual TLS
```

Every InterBridge gets a unique permanent device certificate. The device generates
its permanent key pair locally and submits a CSR. Fleet Provisioning must use
`CreateCertificateFromCsr`; production provisioning must not use a flow that returns
a newly generated permanent private key to the device.

A permanent fleet-wide shared certificate/private key is forbidden.

Permanent device credentials must never appear in logs, normal MQTT payloads, source constants, repository files, QR codes, or `CONTEXT.md`.

---

## 6. Production Credential Provisioning

Use **AWS IoT Fleet Provisioning by Trusted User**.

### 6.1 Product claim

The primary path resolves the physical device via BLE nearby discovery,
not a QR scan - see section 7.1. QR (section 4.3) and manual `setup_code`
entry (section 7.3) are fallback ways to resolve the same device, used
only when discovery alone isn't practical; either way, the flow from
"user is signed in" onward is identical:

```text
User signs into InterApp
        ↓
App discovers InterBridge (nearby BLE - primary)
   or resolves it via QR/manual setup_code (fallback - section 7.3)
        ↓
User confirms this is the physical device they intend to set up
        ↓
Secure BLE session established (section 7.2)
        ↓
Backend validates, over an authenticated application session:
- authenticated user
- device_id (and setup_code, if a fallback path was used)
- current claim/ownership state
        ↓
Backend associates device with user
        ↓
Backend issues a short-lived, backend-authenticated
temporary application claim session (distinct from setup_code
and from the AWS Fleet Provisioning claim below - see section 4)
        ↓
Backend requests a temporary AWS IoT Fleet Provisioning claim
        ↓
Temporary provisioning material is delivered
to the physical device through the secure BLE session
```

The mobile app must never receive AWS administrative credentials. The
temporary application claim session token must expire, must not be
reused for normal device operation, and must never be logged - see
section 4's terminology note for how it differs from `setup_code` and
from the AWS Fleet Provisioning temporary claim.

### 6.2 Fleet Provisioning

```text
InterBridge
    ↓ Wi-Fi
AWS IoT Core
    ↓ temporary trusted-user provisioning claim
Fleet Provisioning MQTT APIs
    ├── device generates permanent key pair locally
    ├── device submits CSR
    ├── AWS issues permanent certificate from CSR
    ├── register Thing
    ├── attach policy/resources
    └── return permanent credential material
            ↓
       InterBridge stores
       permanent credentials
```

After permanent provisioning succeeds, temporary claim credentials are securely
discarded and never reused for normal operation. Documentation and code must use
distinct names for `setup_code`, the temporary application claim session, and the
temporary AWS Fleet Provisioning claim to avoid confusing these three credentials
(see section 4's terminology note).

---

## 7. BLE Provisioning

BLE is the **primary** onboarding transport - not just a first-time/
recovery fallback. Use ESP-IDF Unified Provisioning / Wi-Fi Provisioning
Manager over BLE/Protocomm.

### 7.1 Primary flow: nearby discovery

```text
Device in provisioning mode (auto at boot if unprovisioned,
or after a ~3s button hold if already provisioned - section 8)
        ↓
App discovers InterBridge over BLE (nearby, no QR/code needed)
        ↓
User confirms the physical device (via the advertised name - section 7.4)
        ↓
Secure BLE session established (section 7.2)
        ↓
Backend-authorized temporary claim material delivered (section 6.1)
        ↓
Wi-Fi credentials transferred over the secure session
        ↓
device joins Wi-Fi
        ↓
AWS Fleet Provisioning (section 6.2) - skipped if a permanent
certificate already exists (e.g. re-provisioning just the Wi-Fi network)
        ↓
permanent X.509 credential obtained (or already present)
        ↓
normal AWS IoT Core connection
        ↓
PROVISIONING_COMPLETED
```

The device does not require QR scanning or manual code entry for this
primary flow - see section 7.3 for when those are used instead.

### 7.2 Security mode

Protocol v1 requires the strongest Protocomm security mode the pinned
ESP-IDF version supports cleanly:

```text
Preferred:  Protocomm Security 2
Fallback:   Protocomm Security 1
Forbidden:  plaintext / unsecured provisioning
```

Each physical device uses a unique, high-entropy Proof of Possession
(PoP). The exact ESP-IDF version is pinned by the firmware build, and
provisioning interoperability must be tested against the corresponding
mobile implementation before release. If Security 2 cannot currently be
completed end-to-end because of framework/version limitations, the
firmware must keep the abstraction, document the limitation, and never
silently fall back to plaintext - see CONTEXT.md for the current status
of this integration (the firmware's `BleSecurityMode` enum has no
plaintext value at all, by construction).

`setup_code` and the BLE PoP must not silently be the same value. If
derived from common manufacturing material, use explicit domain
separation and sufficient entropy for the PoP specifically (`setup_code`
itself does not need to be high-entropy - see section 4.2).

### 7.3 Fallback: QR / manual setup_code

QR (section 4.3) and manually typing the 12-digit `setup_code` are
alternate ways for the app/backend to **resolve which physical device**
to open a BLE session with - never independent provisioning
implementations. Once the app has resolved a device by any of the three
methods (nearby discovery, QR, manual entry), it opens the same BLE
session type and the same device-side provisioning coordinator handles
everything from there identically.

### 7.4 BLE advertisement model

While provisioning is active, the device advertises:

```text
device_name              "InterBridge-XXXX" (XXXX = last 4 hex chars of device_id, uppercased)
provisioning_available    true
device identity hint       the same 4-character fragment used in device_name
```

No secret is ever advertised (`setup_code`, PoP, and Wi-Fi credentials
never appear in advertisement data). This is enough for the app to list
compatible nearby devices, tell them apart, and confirm which one the
user is confirming/tapping - see `provisioning/ble_provisioning.h`'s
`BleAdvertisementInfo`/`buildBleAdvertisementInfo()`.

### 7.5 Provisioning state machine

The device-side onboarding lifecycle is tracked by a dedicated
coordinator (`ProvisioningManager`), separate from the intercom
call-flow state machine (`core::State` - BOOT/IDLE/RINGING/IN_CALL/
ERROR). Folding onboarding into the call-flow machine would force it to
grow states unrelated to its own purpose.

```text
NOT_PROVISIONED
        │ (auto at boot, or ~3s button hold if already provisioned)
        ▼
PROVISIONING_AVAILABLE  ──(BLE central connects)──▶  BLE_SESSION_ACTIVE
        │                                                    │
        │                                    (Wi-Fi credentials received)
        │                                                    ▼
        │                                          CONNECTING_WIFI
        │                                                    │ (Wi-Fi connected)
        │                                                    ▼
        │                                          FLEET_PROVISIONING
        │                                     (skipped if cert already stored)
        │                                                    │
        │                                                    ▼
        │                                          CLOUD_CONNECTING
        │                                                    │
        │              (success)                             ▼
        │◀────────────────────────────────────────────  PROVISIONED
        │
        │              (failure at any step, still within
        │               the provisioning window)
        └───────────────────────────────────────────  PROVISIONING_FAILED
                          (recovers back to PROVISIONING_AVAILABLE)
```

`IDLE` (provisioned, not currently in the onboarding flow) is the
device's normal resting state once `PROVISIONED` is reached, or on any
boot where valid Wi-Fi configuration already exists.

`CLOUD_CONNECTING` represents handing off to the device's ordinary MQTT
connect/reconnect logic (section 25) - the provisioning coordinator does
not duplicate that logic or itself verify the first successful MQTT
connection; its job is done once Wi-Fi and an AWS IoT certificate are
both in place.

### 7.6 Provisioning window and recovery

```text
Provisioning window:  5 minutes (from entering PROVISIONING_AVAILABLE)
```

If the window elapses before reaching `PROVISIONED`, BLE advertising
stops and the device returns to normal operation (`IDLE` if it was
already provisioned before this attempt, `NOT_PROVISIONED` otherwise). A
failure at any single step does **not** require a reboot or a fresh
button press to retry - the device recovers to `PROVISIONING_AVAILABLE`
and keeps advertising as long as the overall window hasn't elapsed. This
must cover, at minimum:

```text
BLE session disconnect before credentials arrive   -> resume advertising
Wi-Fi credentials invalid / connect never succeeds  -> caught by the overall window (no shorter dedicated timeout yet - see CONTEXT.md > Technical Debt)
Fleet Provisioning rejection/failure                 -> PROVISIONING_FAILED, then resume advertising
app exits/disconnects mid-setup                        -> same as BLE session disconnect
```

Backend-issued temporary claim/session expiry (section 4/6.1) is a
backend-side concern this firmware does not currently model a dedicated
recovery path for - see CONTEXT.md > Open Questions.

### 7.7 Status indication (LED)

The firmware must expose semantic indications for onboarding feedback,
without this document or the firmware choosing an LED GPIO or final
blink/color design (hardware not finalized):

```text
PROVISIONING_AVAILABLE   advertising, waiting for the app to connect
APP_CONNECTED             a BLE session is active (identification feedback)
PROVISIONING_SUCCESS
PROVISIONING_FAILURE
FACTORY_RESET_WARNING      shown before/during a destructive factory reset (section 9)
```

See `hardware/status_indicator.h`'s `IStatusIndicator`/
`ProvisioningIndication`.

Never log Wi-Fi password, PoP, `setup_code`, temporary application claim
session, temporary provisioning private key, or permanent private key.

---

## 8. Physical Config / Reset Button

```text
CONFIG_RESET_BUTTON_PIN = TBD
```

Direct GPIO reads stay inside the hardware/input layer.

Initial behavior:

```text
short press       no destructive action
hold ~3 seconds   ProvisioningRequested
hold ~10 seconds  FactoryResetRequested
```

Use named constants for thresholds, software debounce, and one-shot triggering while held.

High-level code consumes semantic events and must not depend on GPIO numbers.

Architecture should allow LED/status feedback (section 7.7), but LED GPIO/patterns remain TBD.

`ProvisioningRequested` is how an **already-provisioned** device reopens
onboarding: a normal boot with valid Wi-Fi configuration does not
advertise (section 7.5's `IDLE`), and only this button trigger (not a
remote MQTT command - `ENTER_PROVISIONING` is never remotely executable,
section 18) reopens `PROVISIONING_AVAILABLE` for the 5-minute window
(section 7.6). An **unprovisioned** device advertises automatically at
boot without needing the button at all.

---

## 9. Factory Reset

Factory reset clears user-specific configuration:

```text
Wi-Fi credentials
local user configuration
transient provisioning/session state
```

Stable manufacturing identity survives - `device_id` **and** `setup_code`
(section 4.2), plus the permanent AWS IoT certificate/private key (below).
`FactoryResetCoordinator` (`provisioning/factory_reset_coordinator.h`)
only clears Wi-Fi credentials and the provisioned flag; it never touches
the `device_id`/`setup_code`/certificate/private-key storage keys.

Permanent AWS IoT device credentials should survive an ordinary factory reset by default. Cloud credential revocation/reprovisioning is a separate explicit decommission/recovery operation.

Remote `FACTORY_RESET` is not permitted in protocol v1. Factory reset requires the
physical button hold. Remote decommissioning may revoke cloud authorization and the
certificate, but must not silently perform a destructive local reset.

### 9.1 Ownership transfer and decommissioning

```text
Reconfigure Wi-Fi          preserve certificate and ownership
Ordinary factory reset     preserve certificate; clear Wi-Fi/local user config
Owner removes device       detach ownership; device becomes unclaimed
Ownership transfer         explicit old-owner approval or controlled recovery flow
Certificate compromise     revoke certificate and perform recovery provisioning
Permanent decommission     revoke certificate and prevent further cloud access
```

Removing an owner must invalidate previous user authorization immediately. Transfer
requires a new backend-issued temporary application claim session (section 4/6.1) -
`setup_code` itself is permanent (section 4.2, never regenerated, printed on the
device) and is not the thing that gets reissued; what changes on transfer is the
backend's ownership/claim record and the temporary session used to complete it.

---

## 10. MQTT Connection

Normal communication connects directly to the AWS IoT Core data endpoint using MQTT over TLS.

Required behavior:

```text
TLS server verification
X.509 client authentication
unique ClientId
QoS 0/1
automatic reconnect
bounded exponential backoff + jitter
non-blocking connection state machine
```

Protocol v1 connection parameters:

```text
MQTT version                 MQTT 3.1.1
keepalive                    300 seconds
clean session               true
automatic resubscription    required after every reconnect
custom retained messages    forbidden for commands/responses/events
initial reconnect delay     1 second
maximum reconnect delay     300 seconds
backoff                     exponential with full jitter
maximum custom JSON payload 8 KiB
```

Clean sessions are intentional: delayed physical commands must not be queued by an
old broker session. Durable fleet operations use AWS IoT Jobs; desired persistent
configuration uses Device Shadow. A future move to MQTT 5 requires an explicit
protocol revision and validation against the selected ESP-IDF MQTT client.

Endpoint, root CA and environment metadata must be configuration-driven.

---

## 11. Environment Separation

At minimum:

```text
DEV
PROD
```

Native/local tests use mocks and do not require AWS.

Credentials are environment-specific. Development credentials must never authenticate to production infrastructure.

A future STAGING environment may be added.

---

## 12. Custom MQTT Topic Namespace

Normal broker topics:

```text
interbridge/{device_id}/commands
```

Backend-only ingestion should use AWS IoT Basic Ingest:

```text
$aws/rules/{ingest_rule}/interbridge/{device_id}/events
$aws/rules/{ingest_rule}/interbridge/{device_id}/health
$aws/rules/{response_rule}/interbridge/{device_id}/responses
```

The rule name is infrastructure configuration, not a string duplicated throughout the firmware.

---

## 13. Basic Ingest

Use Basic Ingest for messages that only need to reach AWS IoT Rules/backend actions and do not need another MQTT subscriber to receive the original broker publication.

Good candidates:

```text
events
health telemetry
diagnostic telemetry
command responses
```

Do not use Basic Ingest for commands or messages that must be delivered through the normal MQTT broker.

---

## 14. Common Custom Message Envelope

Custom JSON messages include:

```json
{
  "protocol_version": 1
}
```

Where applicable:

```text
device_id
event_id
command_id
timestamp
uptime_ms
```

Identifiers are 128-bit random values encoded as 32 lowercase hexadecimal
characters. `device_id` and `event_id` (both firmware-generated) carry a
semantic prefix - `ib-` and `evt-` respectively. **`command_id` is
backend-generated and carries no prefix**: it is a bare
`^[0-9a-f]{32}$` value, e.g. `0123456789abcdef0123456789abcdef` - see
the example in section 18. This is a deliberate asymmetry, not an
inconsistency: prefixes exist so a human/log reader can tell at a glance
what generated an ID, and only the device generates `device_id`/
`event_id`. Custom JSON payloads must not exceed 8 KiB.

`device_id` inside JSON is diagnostic information only. Authorization comes from AWS IoT certificate/policy/Thing context.

### 14.1 Timestamp representations differ by message type

This protocol uses **two different, non-interchangeable** timestamp
representations - implementations must not treat them as equivalent or
attempt to parse one as the other:

```text
DeviceEvent.timestamp                UTC ISO-8601 string, e.g. "2026-08-11T14:30:25Z"
DeviceCommand.issued_at/expires_at   Unix epoch seconds, integer, e.g. 1786467600
```

Event `timestamp` is set by the device *if and only if* it has valid
wall-clock time (section 24.1/NTP); otherwise the field is omitted -
never invented. Command `issued_at`/`expires_at` are set by the backend
and are always integers; the firmware never parses a string in those two
fields as a timestamp (see section 18) - a string there is treated as if
the field were entirely absent, which fails command time-safety
validation (`INVALID_TIMESTAMP`).

---

## 15. QoS

| Message | QoS |
|---|---:|
| Commands | 1 |
| Command responses | 1 |
| Important events | 1 |
| Non-critical health telemetry | 0 |
| Shadow/Jobs/Fleet Provisioning | Follow AWS service requirements |

QoS 2 is not required.

Because QoS 1 is at-least-once, command handling must be idempotency-aware.

---

## 16. Device Events

Basic Ingest route:

```text
$aws/rules/{ingest_rule}/interbridge/{device_id}/events
```

Example:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "event_id": "evt-12345",
  "event": "RING_DETECTED",
  "timestamp": "2026-08-11T17:30:25Z",
  "uptime_ms": 123456
}
```

Initial vocabulary:

```text
RING_DETECTED
OFF_HOOK
ON_HOOK
CALL_STARTED
CALL_ENDED
DOOR_OPENED
DOOR_OPEN_FAILED
PROVISIONING_STARTED
PROVISIONING_COMPLETED
PROVISIONING_FAILED
FACTORY_RESET_REQUESTED
OTA_STARTED
OTA_COMPLETED
OTA_FAILED
ERROR
```

Do not emit events whose real semantic source is not implemented yet.

---

## 17. Event Reliability / Outbox

Important events should survive temporary cloud disconnection.

Use a bounded event outbox with stable `event_id`.

Candidate replayable events:

```text
RING_DETECTED
CALL_STARTED
CALL_ENDED
DOOR_OPENED
DOOR_OPEN_FAILED
OTA_FAILED
ERROR
```

Backend ingestion must be idempotent by `event_id`.

An in-memory queue is acceptable during development. Production firmware must use
a bounded NVS-backed outbox for `RING_DETECTED`, `CALL_STARTED`, `CALL_ENDED`,
`DOOR_OPENED`, `DOOR_OPEN_FAILED`, `OTA_FAILED`, and security-relevant `ERROR`
events. The queue must define deterministic eviction, wear-aware writes, and replay
with the original stable `event_id`.

---

## 18. Commands

Topic:

```text
interbridge/{device_id}/commands
```

Base command:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "command_id": "0123456789abcdef0123456789abcdef",
  "command": "OPEN_DOOR",
  "parameters": {},
  "issued_at": 1786467600,
  "expires_at": 1786467630
}
```

`command_id` is exactly 32 lowercase hexadecimal characters, no prefix
(section 14.1). `issued_at`/`expires_at` are Unix epoch seconds
(integers), **not** ISO-8601 strings - see section 14.1 for why command
timestamps and event timestamps use different representations. A command
whose `issued_at`/`expires_at` are sent as strings is treated by the
firmware as if those fields were absent, which fails time-safety
validation (`INVALID_TIMESTAMP`) rather than being parsed.

Phase 2D remote allowlist:

```text
OPEN_DOOR
```

`RESTART`, `ENTER_PROVISIONING`, `FACTORY_RESET`, call commands, and unknown
commands are not remotely executable. `parameters` must be present and exactly an
empty object. DTMF, key, GPIO, mode, relay, and pulse-duration fields are rejected.

All remote commands include `issued_at` and `expires_at`. The device must reject a
command when its clock is not valid, the command is expired, the validity interval
exceeds the allowed maximum, either timestamp is missing/malformed, or its
identifier has already been processed. The device accepts `issued_at` at most 5
seconds in the future.

Initial maximum validity:

```text
OPEN_DOOR  30 seconds
```

The firmware must establish trustworthy wall-clock time before accepting remote
sensitive commands. Commands are never retained.

Reserved for future call/audio logic:

```text
ANSWER_CALL
REJECT_CALL
END_CALL
```

Firmware update is not primarily a custom `UPDATE_FIRMWARE` command; OTA uses AWS IoT Jobs.

---

## 19. Command Responses

Basic Ingest route:

```text
$aws/rules/{response_rule}/interbridge/{device_id}/responses
```

Statuses:

```text
ACCEPTED
COMPLETED
FAILED
REJECTED
```

Example:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "command_id": "0123456789abcdef0123456789abcdef",
  "command": "OPEN_DOOR",
  "status": "ACCEPTED"
}
```

Phase 2D terminal response:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "command_id": "0123456789abcdef0123456789abcdef",
  "command": "OPEN_DOOR",
  "status": "REJECTED",
  "error": {
    "code": "CAPABILITY_DISABLED",
    "message": "Door opening capability is disabled"
  }
}
```

---

## 20. Duplicate Command Protection

Maintain a bounded recent-command cache by `command_id`.

On duplicate delivery:

- never execute the physical/destructive action twice;
- return the previous known result when available.

Mandatory in protocol v1 for:

```text
OPEN_DOOR
```

For production, the recent-command cache for `OPEN_DOOR` must survive reboot for at
least the maximum command-validity window. Development builds may begin with an
in-memory implementation behind the same abstraction.

### 20.1 Response and timeout contract

- Publish `ACCEPTED` only after validation and before beginning an asynchronous action.
- Publish one terminal result: `COMPLETED`, `FAILED`, or `REJECTED`.
- In Phase 2D, `ACCEPTED` means accepted only for processing. With the default
  `DISABLED` capability, `OPEN_DOOR` is followed by
  `REJECTED/CAPABILITY_DISABLED`; it never produces physical success.
- `OPEN_DOOR` should normally emit its terminal result within 5 seconds.
- `RESTART` may publish `ACCEPTED` before reboot; the subsequent successful cloud
  reconnect is the authoritative completion signal.
- The backend persists the command before publishing it, correlates responses by
  `command_id`, and treats duplicate responses idempotently.
- Absence of a response before the backend deadline is `TIMED_OUT`; it does not
  authorize blindly replaying an expired physical command.

---

## 21. Error Codes

Canonical protocol v1 error code set, with the party that originates
each one. "Device" means the InterBridge firmware itself constructs a
`ProtocolError` with this code today. "Backend"/"Application" means the
code exists in the shared contract for the backend/app to use (e.g. in a
response the backend synthesizes on the device's behalf, such as a
timeout, or a rejection the backend makes before a message ever reaches
the device) - **the firmware does not currently construct these**, and
must not be made to fabricate them just to look more complete.

| Code | Origin | Meaning |
|---|---|---|
| `INVALID_PAYLOAD` | Device | Malformed JSON or missing/wrong-type required field |
| `PAYLOAD_TOO_LARGE` | Device | Payload exceeds the 8 KiB limit (section 14) |
| `UNSUPPORTED_PROTOCOL_VERSION` | Device | `protocol_version` is missing or not `1` |
| `UNKNOWN_COMMAND` | Device | `command` string is not a recognized command type |
| `COMMAND_NOT_ALLOWED` | Device | Recognized command, but not remotely executable (`ENTER_PROVISIONING`, `FACTORY_RESET`, reserved call commands) or not valid in the current device state |
| `COMMAND_EXPIRED` | Device | `now > expires_at` |
| `CLOCK_NOT_TRUSTWORTHY` | Device | Device has no valid wall-clock time yet (NTP not implemented - section 24.1); no time-sensitive command can be accepted |
| `INVALID_TIMESTAMP` | Device | `issued_at`/`expires_at` missing, not an integer (e.g. sent as an ISO-8601 string), `expires_at <= issued_at`, or the validity window exceeds the command's maximum (section 18) |
| `DEVICE_BUSY` | Device | Reserved; not currently produced by the firmware |
| `NOT_PROVISIONED` | Backend | Reserved for the backend to reject a command aimed at a device that hasn't completed provisioning; not produced by the firmware |
| `WIFI_UNAVAILABLE` | Backend | Reserved for the backend to describe a device it knows to be offline at the Wi-Fi level; not produced by the firmware |
| `CLOUD_UNAVAILABLE` | Backend/Application | Reserved for the backend/app to describe an AWS IoT Core outage; not produced by the firmware |
| `DOOR_OUTPUT_FAILURE` | Device | `IHardwareIO::setDoorOutput()` reported failure (always true today - see CONTEXT.md, the hardware layer is a stub) |
| `CAPABILITY_DISABLED` | Device | `OPEN_DOOR` was valid but door-opening capability is `DISABLED` |
| `OTA_DOWNLOAD_FAILED` | Device | `IOtaPlatform::downloadAndHash()` failed |
| `OTA_VALIDATION_FAILED` | Device | SHA-256 or signature check failed (signature always fails today - no signing scheme exists yet, see section 30) |
| `OTA_INSTALL_FAILED` | Device | Install, reboot, or boot-confirmation step failed |
| `PROVISIONING_FAILED` | Device | Reserved for a `ProvisioningManager` failure/timeout path - not implemented yet (it can currently get stuck in `ConnectingWifi` with no failure signal, see CONTEXT.md) |
| `INTERNAL_ERROR` | Device | Catch-all for an unexpected firmware-side failure |

Errors must not expose secrets.

---

## 22. AWS IoT Device Shadow

Use a **named Device Shadow** for persistent device state/configuration.

Recommended shadow name:

```text
interbridge
```

Prefix:

```text
$aws/things/{device_id}/shadow/name/interbridge/...
```

Reported example:

```json
{
  "state": {
    "reported": {
      "firmware_version": "0.1.0",
      "hardware_version": "1.0",
      "intercom_state": "IDLE",
      "wifi_rssi": -54,
      "uptime_ms": 3812000,
      "provisioned": true,
      "health_interval_s": 3600
    }
  }
}
```

`intercom_state` (here and in Health Telemetry, section 24) is limited to
a closed, canonical set - see section 22.1.

Desired example:

```json
{
  "state": {
    "desired": {
      "health_interval_s": 3600
    }
  }
}
```

### 22.1 Canonical intercom_state values

```text
IDLE
RINGING
OFF_HOOK
IN_CALL
ERROR
```

Publishers must use exactly one of these five strings - never an
arbitrary/internal state name (the firmware's internal state machine has
a `BOOT` state used only for diagnostic logging; it is intentionally
mapped to `IDLE` rather than ever appearing here, since a device
publishing telemetry has always already finished booting).

**`OFF_HOOK` is part of the canonical vocabulary but is not currently
reachable.** The firmware's core state machine transitions directly from
`RINGING` to a combined "in call" state on the off-hook signal - there is
no distinct resting "picked up, not yet in a call" state to report
honestly today. Do not publish `OFF_HOOK` by inferring it indirectly
(e.g. from a transient event) - only publish it once the firmware
genuinely has a state that means that. See `CONTEXT.md` > Decisions.

Possible future desired config:

```text
ring_timeout_ms
door_open_duration_ms
audio_volume
health_interval_s
```

Secrets never belong in Device Shadow.

Device handles supported `/delta` changes, applies them, then updates `reported`.

Unknown future fields must not crash firmware.

---

## 23. Connectivity / Availability

AWS IoT lifecycle/connectivity events are authoritative for cloud-side device online/offline state.

Backend should consume AWS IoT connect/disconnect lifecycle events instead of relying only on a custom Last Will.

Lifecycle events may be duplicated, delayed, or delivered out of order. The backend
must compare the AWS lifecycle event version/sequence information for the same
`clientId` and must not let an older event overwrite newer connectivity state.

Protocol v1 does not publish a custom Last Will. If a later version adds one, it is
supplementary and not authoritative over ordered AWS lifecycle state.

Health telemetry supplements connectivity status.

---

## 24. Health Telemetry

Basic Ingest route:

```text
$aws/rules/{ingest_rule}/interbridge/{device_id}/health
```

Initial cadence:

```text
3600 seconds (1 hour)
```

Health is also published immediately after boot/reconnect and on material changes
such as firmware update, persistent hardware error, critically low heap, or severe
Wi-Fi degradation. Do not use periodic health as the authoritative online/offline
signal and do not write every report to long-term storage unless required.

Example:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "firmware_version": "0.1.0",
  "intercom_state": "IDLE",
  "uptime_ms": 18372000,
  "wifi_rssi": -54,
  "free_heap": 123456
}
```

Do not publish unnecessary high-frequency diagnostics in production.

---

## 25. Reconnect Strategy

On disconnect:

```text
detect disconnect
keep core intercom logic running
bounded exponential backoff + jitter
re-establish TLS/MQTT
restore required subscriptions
resynchronize Device Shadow
check pending AWS IoT Jobs
flush important event outbox
```

No retry storms and no blocking sleep loops in core firmware.

---

## 26. AWS IoT Policies

Least privilege is mandatory.

A device may only:

- connect as its expected ClientId;
- publish to its own permitted topics;
- subscribe/receive only its own command topic;
- access its own Device Shadow;
- access its own Jobs topics;
- access provisioning topics only while using temporary provisioning credentials.

One device must never control or subscribe to another device's private topics.

Policy resource paths must derive the permitted identity from the authenticated
Thing/certificate context and enforce `ClientId == ThingName == device_id`. Payload
`device_id` is never used for authorization. Basic Ingest permission is limited to
the exact environment rule prefixes for that device.

---

## 27. Application Backend Boundary

The InterApp never receives permanent device private keys or permanent device X.509 credentials.

Conceptual command flow:

```text
InterApp
   ↓
authenticated backend
   ↓
authorization: user can control device?
   ↓
AWS IoT publish
   ↓
interbridge/{device_id}/commands
   ↓
InterBridge
```

Human authentication is separate from device authentication.

AWS application services may include:

```text
Amazon Cognito
API Gateway
Lambda
application database/storage
```

Detailed app database schema is outside this firmware protocol unless it affects the device contract.

### 27.1 Local communication boundary

When the InterApp and InterBridge are on the same trusted LAN, a separate local
HTTPS/WebSocket protocol may provide low-latency status and control without a cloud
round trip. Local discovery, pairing, authorization, certificate pinning and replay
protection must be defined in a separate contract before that path is enabled.

The local path must preserve the same authorization intent, command identifiers,
expiry and idempotency guarantees as the cloud path. Merely being connected to the
same Wi-Fi is not sufficient authorization. Audio remains outside this document.

---

## 28. AWS IoT Jobs

Use AWS IoT Jobs for durable remote fleet operations.

Initial primary use:

```text
OTA firmware update
```

Potential future uses:

```text
certificate rotation
diagnostics
maintenance
controlled reboot
```

The firmware must use AWS Jobs reserved MQTT topics rather than reimplementing Jobs through custom application topics.

---

## 29. OTA

OTA architecture:

```text
AWS IoT Jobs
+
Amazon S3
+
digitally signed firmware
+
ESP32 OTA partitions/rollback
```

Flow:

```text
CI/build
  ↓
firmware image
  ↓
signature + integrity metadata
  ↓
S3
  ↓
AWS IoT Job
  ↓
device receives job
  ↓
HTTPS/S3 dynamically presigned download
  ↓
verify integrity/authenticity
  ↓
install inactive OTA partition
  ↓
reboot/self-test
  ├── OK   → mark valid
  └── FAIL → rollback previous image
```

Prefer HTTPS/S3 presigned download initially. Do not embed an ordinarily generated
short-lived URL directly in a long-lived Job document. Use the AWS IoT Jobs S3
presigned-URL placeholder mechanism (or an explicitly authorized backend endpoint)
so a fresh, short-lived URL is generated when the device retrieves/executes the Job.
Job control/status remains through AWS IoT Jobs MQTT APIs.

Production requirements:

```text
TLS download
SHA-256/integrity validation
digital signature validation
version validation
partitioned install
boot validation
rollback
job progress/status reporting
```

Application diagnostic events may also publish:

```text
OTA_STARTED
OTA_COMPLETED
OTA_FAILED
```

AWS IoT Jobs remains the authoritative remote job state.

---

## 30. Firmware Signing

Production firmware must be signed.

Signing private keys must never live in:

```text
firmware repository
device
public S3 location
mobile app
```

Signing occurs in a controlled CI/release or AWS-supported code-signing workflow.

The device trusts only public verification material required by the ESP-IDF secure-boot/OTA design.

---

## 31. Secure Boot / Flash Encryption / Rollback

Production target:

```text
Secure Boot V2
Flash Encryption
signed OTA
OTA rollback
```

Test OTA rollback before fleet deployment.

Do not enable irreversible anti-rollback/eFuse policy casually during development.

---

## 32. Persistent Storage

Persist:

```text
Wi-Fi credentials
stable device_id
permanent AWS IoT certificate
permanent device private key
AWS IoT endpoint/environment
provisioning state
local configuration
```

Use a storage abstraction so native tests can use memory.

ESP implementation may use NVS/secure storage.

Never log secrets.

---

## 33. Protocol Versioning

Every custom application message includes:

```json
{
  "protocol_version": 1
}
```

Unsupported versions fail safely with:

```text
UNSUPPORTED_PROTOCOL_VERSION
```

Consumers tolerate additional unknown fields.

AWS reserved topics/contracts for Shadow, Jobs and Fleet Provisioning follow AWS service contracts and are not governed by this custom version field.

---

## 34. Logging

Log:

```text
boot
firmware version
Wi-Fi state
AWS IoT connection state
MQTT subscribe/reconnect
command type/id/result
event publish result
Shadow sync
provisioning lifecycle (state transitions, section 7.5)
Fleet Provisioning lifecycle
Jobs lifecycle
OTA lifecycle
factory reset lifecycle
```

Never log:

```text
Wi-Fi password
setup_code
temporary application claim session token
BLE PoP
temporary provisioning private key
permanent device private key
AWS administrative credentials
```

---

## 35. Test Requirements

Native tests should cover all non-hardware/non-cloud logic.

Test at minimum:

- command/response/Basic-Ingest topic building;
- command parsing and malformed payloads;
- unsupported protocol versions;
- unknown commands;
- accepted/completed/failed/rejected command flow;
- duplicate command suppression;
- command expiry, invalid clock and oversized validity-window rejection;
- event outbox queue/flush/bounded capacity/stable IDs;
- production NVS outbox replay and deterministic eviction;
- button debounce and hold thresholds;
- factory reset preserving identity/device credential/setup_code;
- provisioning coordinator state flow (nearby-discovery path, BLE session
  start/end, credential receipt, Fleet Provisioning success/failure and
  retry, provisioning window expiry, status indication);
- Shadow reported serialization and desired parsing;
- reconnect/backoff behavior with fake time;
- lifecycle event deduplication/out-of-order handling in backend tests;
- outbox flush + shadow resync + Jobs check after reconnect;
- OTA coordinator success/failure/rollback using fake updater.

Do not use real sleeps in deterministic native tests.

---

## 36. Current Decisions

| Area | Decision |
|---|---|
| Device cloud | AWS IoT Core |
| Transport | MQTT over TLS |
| MQTT v1 profile | MQTT 3.1.1, keepalive 300 s, clean session, no custom retained messages |
| Device auth | unique X.509 per device + mTLS |
| Permanent private key | generated on-device; never leaves device |
| Permanent certificate request | Fleet Provisioning with `CreateCertificateFromCsr` |
| Thing name | stable `device_id` |
| MQTT ClientId | stable `device_id` |
| Onboarding UX | BLE-first: nearby discovery is primary; QR/manual `setup_code` are fallback identity-resolution only |
| BLE framework | ESP-IDF Unified Provisioning / Protocomm |
| BLE security | Protocomm Security 2 preferred, Security 1 fallback, unique high-entropy PoP; plaintext forbidden |
| Human-facing onboarding ID | `setup_code`: 12 decimal digits, permanent, not a secret/credential |
| Provisioning window | 5 minutes; unprovisioned devices advertise automatically at boot, provisioned devices reopen via a ~3s button hold |
| Product ownership claim | app-authenticated claim session, resolved via nearby BLE discovery (primary) or QR/manual `setup_code` (fallback) |
| Permanent IoT credential issuance | AWS Fleet Provisioning by Trusted User |
| Device state/config | named AWS IoT Device Shadow |
| Online/offline | AWS IoT lifecycle/connectivity events |
| Backend-only ingestion | AWS IoT Basic Ingest |
| Device commands | normal MQTT broker topics |
| Command responses | AWS IoT Basic Ingest |
| Remote command safety | issued/expiry timestamps, deduplication, never retained |
| Health cadence | hourly plus material-change events |
| OTA orchestration | AWS IoT Jobs |
| OTA artifacts | Amazon S3 |
| Initial OTA transfer | dynamically presigned HTTPS/S3 URL from Jobs execution context |
| Firmware authenticity | digital signature |
| OTA recovery | ESP32 rollback |
| Production hardening | Secure Boot V2 + Flash Encryption |
| Audio | separate transport |
| Physical config/reset button | required; GPIO TBD |
| Factory reset | preserves stable manufacturing/device identity and permanent IoT credential by default |
| Remote destructive reset | forbidden in protocol v1; physical confirmation required |

---

## 37. Still Open

### 37.1 Backend/infrastructure blockers

These must be decided before creating the production-shaped AWS infrastructure:

- final AWS Region, accounts and DEV/PROD environment layout;
- exact IoT Policy templates and Fleet Provisioning template;
- final Basic Ingest rule names and actions;
- application backend/API contracts;
- application database access patterns and schema;
- Cognito user/identity-pool design;
- ownership transfer, support-assisted recovery and certificate-rotation API details;
- command persistence, backend deadlines and InterApp result-delivery mechanism;
- mobile push-notification provider integration through AWS;
- local communication/discovery/pairing contract;
- WebRTC signaling and TURN fallback architecture;
- final CI secret-management and firmware-signing workflow;
- observability retention, cost budgets and alarms.

### 37.2 Hardware/manufacturing-dependent

- exact ESP32-C3 board/module;
- final GPIO assignments;
- config/reset button GPIO;
- status LED GPIO;
- intercom electrical interface;
- ring detection electronics;
- off-hook/on-hook detection electronics;
- door-release circuit, active level and pulse duration;
- audio hardware;
- audio codec;
- audio transport protocol;
- final secure-storage/eFuse manufacturing procedure;
- production anti-rollback eFuse policy.

### 37.3 Mobile/hardware validation required (BLE onboarding)

- whether Protocomm Security 2 is cleanly supported by the pinned
  ESP-IDF version, or whether Security 1 is the production fallback;
- BLE provisioning interoperability against the actual mobile
  implementation (nearby discovery UX, PoP exchange, credential
  transfer);
- temporary application claim session design/expiry (backend + app,
  section 4/6.1) - not modeled in firmware beyond receiving whatever
  Fleet Provisioning claim material arrives over the BLE session;
- real advertising behavior/range for the `InterBridge-XXXX` device name
  (section 7.4) on the pinned ESP32-C3 BLE stack.

---

## 38. Implementation Status Vocabulary

Use:

```text
Defined
Implemented
Stub
Provisional
Not implemented
Hardware-dependent
Needs validation
Reserved
```

Defining this document does not mean AWS connectivity, provisioning, OTA, BLE, or physical hardware behavior is already implemented or validated.


## Phase 2D concrete ESP32 transport profile

The ESP32 implementation uses MQTT 3.1.1 through `256dpi/MQTT` and mTLS through
`WiFiClientSecure`, connecting directly to the locally configured AWS IoT Data ATS
endpoint (never `DescribeEndpoint`). Client ID is exactly `device_id`. Keepalive is 30 s
and client timeout is 1500 ms; no custom Last Will is set. It subscribes only to
`interbridge/{device_id}/commands` at QoS 1, with no wildcard, and rejects wrong-topic
or greater-than-8-KiB deliveries before protocol parsing. Command results publish to
the existing Basic Ingest response topic at QoS 1 with retain disabled. Reconnection is
Wi-Fi-gated and non-blocking with bounded exponential full jitter, and each new session
gets one subscription attempt (a failed subscription remains fail-closed and can be
retried by the loop without duplicating a successful subscription).

Credential PEM values are read only through `DeviceCredentialStore`; errors are
sanitized and raw payloads and private keys are never logged. DEV inputs live only in
the Git-ignored local header documented in the README. No real AWS/physical validation
was performed for this code change. `OPEN_DOOR` remains a deliberately non-actuating
`ACCEPTED` followed by `REJECTED/CAPABILITY_DISABLED`; `COMPLETED` and physical action
statuses are forbidden in this phase.
