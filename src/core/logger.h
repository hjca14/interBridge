#pragma once

namespace interbridge {

// Minimal structured logging so call sites do not scatter Serial.print /
// std::printf calls throughout the codebase. Output is line-based and
// consistently prefixed, e.g.:
//
//   [INFO] Booting InterBridge
//   [STATE] IDLE -> RINGING
//   [EVENT] OFF_HOOK
//   [ERROR] WiFi connection failed
//
// The actual destination (Serial on the ESP32, stdout on native builds)
// is a swappable sink so this module has no Arduino dependency and stays
// testable on the host.
class Logger {
public:
    using SinkFn = void (*)(const char* line);

    // Overrides the output sink. Defaults to Serial on the esp32-c3
    // environment and to stdout on the native environment.
    static void setSink(SinkFn sink);

    static void info(const char* message);
    static void warn(const char* message);
    static void error(const char* message);

    // Convenience helpers for the two most frequent structured log lines.
    static void stateTransition(const char* from, const char* to);
    static void event(const char* eventName);

private:
    static void write(const char* prefix, const char* message);

    static SinkFn sink_;
};

} // namespace interbridge
