#include "dev_command_diagnostics.h"

#include <Arduino.h>

namespace interbridge {

void logCommandDiagnostic(const char* prefix, const CommandDiagnostic& event) {
    switch (event.stage) {
        case CommandDiagnosticStage::Received:
            // No seq yet here - it is assigned once the command reaches the
            // front of the queue and starts processing (see the stages
            // below), not at raw MQTT delivery time.
            Serial.printf("%s command received\n", prefix);
            break;
        case CommandDiagnosticStage::ValidationPassed:
            Serial.printf("%s time validation ok seq=%lu age_s=%lld remaining_s=%lld\n", prefix,
                          static_cast<unsigned long>(event.commandSeq),
                          static_cast<long long>(event.ageSeconds),
                          static_cast<long long>(event.remainingSeconds));
            break;
        case CommandDiagnosticStage::Rejected:
            Serial.printf("%s command rejected seq=%lu code=%s\n", prefix,
                          static_cast<unsigned long>(event.commandSeq), event.safeCode);
            break;
        case CommandDiagnosticStage::AcceptedPublished:
            Serial.printf("%s ACCEPTED published seq=%lu\n", prefix,
                          static_cast<unsigned long>(event.commandSeq));
            break;
        case CommandDiagnosticStage::AcceptedPending:
        case CommandDiagnosticStage::AcceptedPublishFailed:
            Serial.printf("%s ACCEPTED publish failed; still queued seq=%lu\n", prefix,
                          static_cast<unsigned long>(event.commandSeq));
            break;
        case CommandDiagnosticStage::TerminalPublished:
            // event.safeCode here is the device's own terminal status/error
            // code (e.g. CAPABILITY_DISABLED), never a transport/publish
            // artifact.
            Serial.printf("%s terminal published seq=%lu code=%s\n", prefix,
                          static_cast<unsigned long>(event.commandSeq), event.safeCode);
            break;
        case CommandDiagnosticStage::TerminalDeferred:
            // ACCEPTED already published; this is an intentional
            // one-iteration defer, not a failure - no publish was even
            // attempted yet for the terminal.
            Serial.printf("%s terminal deferred (queued for next iteration) seq=%lu code=%s\n", prefix,
                          static_cast<unsigned long>(event.commandSeq), event.safeCode);
            break;
        case CommandDiagnosticStage::TerminalQueuedBehindAccepted:
            // ACCEPTED itself failed (see the AcceptedPending line above);
            // the terminal is only queued behind it, not attempted yet.
            Serial.printf("%s terminal queued behind pending ACCEPTED seq=%lu code=%s\n", prefix,
                          static_cast<unsigned long>(event.commandSeq), event.safeCode);
            break;
        case CommandDiagnosticStage::TerminalPublishFailed:
            // A real publish attempt happened and failed - the transport
            // layer already logged the sanitized mqtt_err=N for it.
            Serial.printf("%s terminal publish failed; still queued seq=%lu code=%s\n", prefix,
                          static_cast<unsigned long>(event.commandSeq), event.safeCode);
            break;
    }
}

} // namespace interbridge
