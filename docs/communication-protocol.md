# InterBridge Communication Protocol

**Status:** Draft v1.2 — AWS IoT Core architecture, firmware-alignment pass  
**Protocol version:** `1`  
**Target firmware:** InterBridge Firmware `0.1.x`  
**Primary target:** ESP32-C3  
**Cloud device plane:** AWS IoT Core  
**Primary transport:** MQTT over TLS with mutual X.509 authentication

**v1.2 note:** this revision reconciles this document with the firmware
implementation (`src/protocol/messages.h`) - command timestamp format,
the full error code set with per-code origin, the closed
`intercom_state` vocabulary, and the QR claim-code URI format. No
protocol semantics changed; this pass only removed ambiguity/format
divergences between the two. It does **not** implement AWS IoT Core,
real MQTT/TLS, NTP, BLE, real NVS, Lambda, API Gateway, or Cognito - see
`CONTEXT.md` for exactly what remains stubbed ahead of Phase 1.

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

First-time/recovery provisioning uses BLE:

```text
InterApp
   │
   │ BLE + secure provisioning
   ▼
InterBridge
```

The firmware must keep cloud integration behind abstractions so intercom, GPIO, state-machine and audio logic do not directly depend on AWS calls.

---

## 3. Core Decisions

- AWS IoT Core is the device gateway.
- MQTT over TLS is the device control-plane transport.
- Each device authenticates with its own X.509 certificate/private key using mTLS.
- The permanent key pair is generated on-device; the private key never leaves the InterBridge.
- AWS IoT Thing name and MQTT ClientId use the stable `device_id`.
- BLE provisioning uses ESP-IDF Unified Provisioning / Protocomm.
- BLE provisioning uses a unique PoP per physical device.
- Product ownership claim uses QR `device_id + claim_code`.
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

Each InterBridge has a stable product identity:

```text
device_id
hardware_version
claim_code
```

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
enumerate at fleet scale.

### 4.1 QR code format

The QR code encodes a single URI in the canonical form:

```text
interbridge://claim?v=1&device_id=ib-<32 lowercase hex chars>&claim_code=<secret>
```

Example:

```text
interbridge://claim?v=1&device_id=ib-0123456789abcdef0123456789abcdef&claim_code=9f2a7c1e4b3d
```

Validation rules the app (and any backend that parses a scanned code)
must enforce:

```text
scheme                must be exactly "interbridge"
host                  must be exactly "claim"
v                     must be exactly "1"
device_id             must match ^ib-[0-9a-f]{32}$
claim_code             required, must not be empty
duplicate query params  must be rejected (e.g. two "device_id" values)
parameter values         must be percent-decoded before validation
```

`claim_code` is a secret: it must never appear in logs, crash reports, or
analytics events, on either the app or backend side. It is a **product
ownership claim** credential only - it authorizes claiming a physical
unit in the app, and must never be confused with, derived into, or
accepted in place of a permanent AWS IoT device certificate/private key
(section 6).

The firmware does not scan or parse this QR code - it only appears on
the physical device/packaging for the app to read. No QR
scanner/parser is implemented in this repository, and none is needed for
Phase 1.

It must never contain permanent private keys, permanent AWS IoT credentials, or backend administrative secrets.

The product `claim_code` must have at least 128 bits of cryptographically secure
randomness, be single-use for initial ownership claim, be rate-limited at the API,
and be stored by the backend only as a salted password hash or equivalent derived
representation. A new ownership claim requires an explicit transfer/recovery flow;
the original code must not silently become reusable.

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

```text
User signs into InterApp
        ↓
User scans InterBridge QR
        ↓
Backend validates:
- authenticated user
- device_id
- product claim_code
- current claim/ownership state
        ↓
Backend associates device with user
        ↓
Backend requests temporary AWS IoT Fleet Provisioning claim
        ↓
Temporary provisioning material is delivered
to the physical device through the secure BLE session
```

The mobile app must never receive AWS administrative credentials.

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
distinct names for the product ownership `claim_code` and the temporary AWS Fleet
Provisioning claim to avoid confusing the two credentials.

---

## 7. BLE Provisioning

Use ESP-IDF Unified Provisioning / Wi-Fi Provisioning Manager over BLE/Protocomm.

Protocol v1 uses ESP-IDF Protocomm Security 1 with a unique, high-entropy Proof of
Possession per physical device. The exact ESP-IDF version is pinned by the firmware
build, and provisioning interoperability must be tested against the corresponding
mobile implementation before release.

The product `claim_code` and BLE PoP must not silently be the same secret. If derived from common manufacturing material, use explicit domain separation and sufficient entropy.

Provisioning lifecycle:

```text
Unprovisioned device
        ↓
BLE provisioning mode
        ↓
InterApp discovers InterBridge
        ↓
secure session established
        ↓
Wi-Fi credentials transferred
        ↓
temporary AWS Fleet Provisioning material transferred
        ↓
device joins Wi-Fi
        ↓
AWS Fleet Provisioning
        ↓
permanent X.509 credential obtained
        ↓
normal AWS IoT Core connection
        ↓
PROVISIONING_COMPLETED
```

Use custom Protocomm endpoints if extra onboarding metadata is needed.

Never log Wi-Fi password, PoP, claim code, temporary provisioning private key, or permanent private key.

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

Architecture should allow LED/status feedback, but LED GPIO/patterns remain TBD.

---

## 9. Factory Reset

Factory reset clears user-specific configuration:

```text
Wi-Fi credentials
local user configuration
transient provisioning/session state
```

Stable manufacturing identity survives.

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
must issue new claim material; the original product `claim_code` is never re-enabled.

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
  "command_id": "0123456789abcdef0123456789abcdef",
  "command": "OPEN_DOOR",
  "issued_at": 1786467600,
  "expires_at": 1786467610,
  "payload": {}
}
```

`command_id` is exactly 32 lowercase hexadecimal characters, no prefix
(section 14.1). `issued_at`/`expires_at` are Unix epoch seconds
(integers), **not** ISO-8601 strings - see section 14.1 for why command
timestamps and event timestamps use different representations. A command
whose `issued_at`/`expires_at` are sent as strings is treated by the
firmware as if those fields were absent, which fails time-safety
validation (`INVALID_TIMESTAMP`) rather than being parsed.

Initial commands:

```text
OPEN_DOOR
RESTART
```

`ENTER_PROVISIONING` and `FACTORY_RESET` require physical button confirmation and
are not remotely executable commands in protocol v1.

All remote commands include `issued_at` and `expires_at`. The device must reject a
command when its clock is not valid, the command is expired, the validity interval
exceeds the allowed maximum, either timestamp is missing/malformed, or its
identifier has already been processed.

Initial maximum validity:

```text
OPEN_DOOR  10 seconds
RESTART    60 seconds
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
  "status": "COMPLETED"
}
```

Failure:

```json
{
  "protocol_version": 1,
  "device_id": "ib-0123456789abcdef0123456789abcdef",
  "command_id": "0123456789abcdef0123456789abcdef",
  "command": "OPEN_DOOR",
  "status": "FAILED",
  "error": {
    "code": "DOOR_OUTPUT_FAILURE",
    "message": "Door output could not be activated"
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
Fleet Provisioning lifecycle
Jobs lifecycle
OTA lifecycle
factory reset lifecycle
```

Never log:

```text
Wi-Fi password
claim_code
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
- factory reset preserving identity/device credential;
- provisioning coordinator state flow;
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
| Initial provisioning | BLE |
| BLE framework | ESP-IDF Unified Provisioning / Protocomm |
| BLE security | Protocomm Security 1 + unique high-entropy PoP |
| Product ownership claim | QR `device_id + claim_code` |
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
