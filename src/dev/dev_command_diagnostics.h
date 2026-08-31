#pragma once

#include "../protocol/remote_command_processor.h"

namespace interbridge {

// Logs one CommandDiagnostic event to Serial, shared verbatim between
// esp32-c3-dev-mqtt and esp32-c3-dev-ring-simulator so their identical
// diagnostic wording (see docs/mqtt-dev-smoke-test.md's "terminal
// deferred/queued behind accepted/publish failed/published" distinction)
// can never drift apart between the two entry points. `prefix` is the
// caller's own bracketed log tag (e.g. "[DEV MQTT]", "[DEV RING]").
// Arduino-only (Serial) - excluded from the native build, same as the two
// *_main.cpp entry points that call it (see platformio.ini).
void logCommandDiagnostic(const char* prefix, const CommandDiagnostic& event);

} // namespace interbridge
