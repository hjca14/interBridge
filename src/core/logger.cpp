#include "logger.h"

#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace interbridge {

namespace {

#ifdef ARDUINO
void serialSink(const char* line) {
    Serial.println(line);
}
#else
void stdoutSink(const char* line) {
    std::printf("%s\n", line);
}
#endif

} // namespace

#ifdef ARDUINO
Logger::SinkFn Logger::sink_ = serialSink;
#else
Logger::SinkFn Logger::sink_ = stdoutSink;
#endif

void Logger::setSink(SinkFn sink) {
    sink_ = sink;
}

void Logger::write(const char* prefix, const char* message) {
    if (!sink_) {
        return;
    }
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s %s", prefix, message);
    sink_(buffer);
}

void Logger::info(const char* message) {
    write("[INFO]", message);
}

void Logger::warn(const char* message) {
    write("[WARN]", message);
}

void Logger::error(const char* message) {
    write("[ERROR]", message);
}

void Logger::stateTransition(const char* from, const char* to) {
    char message[128];
    std::snprintf(message, sizeof(message), "%s -> %s", from, to);
    write("[STATE]", message);
}

void Logger::event(const char* eventName) {
    write("[EVENT]", eventName);
}

} // namespace interbridge
