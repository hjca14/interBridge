#include <Arduino.h>

#include "audio/audio.h"
#include "core/events.h"
#include "core/logger.h"
#include "core/state_machine.h"
#include "core/version.h"
#include "hardware/gpio.h"
#include "intercom/intercom.h"
#include "network/protocol.h"
#include "network/wifi.h"

using namespace interbridge;

namespace {

Esp32GpioHardware hardware;
Intercom intercom(hardware);
StateMachine stateMachine;
WifiManager wifiManager;
NullAudioIO audio;      // Placeholder until audio hardware is defined.
NullProtocol protocol;  // Placeholder until the communication protocol is chosen.

void onStateTransition(State from, State to) {
    Logger::stateTransition(toString(from), toString(to));
}

void initializeLogging() {
    Serial.begin(115200);
    Logger::info("Booting InterBridge");
    Logger::info("Firmware version: " FIRMWARE_VERSION);
}

void initializeHardware() {
    // Esp32GpioHardware is currently a stub: GPIO mapping and electrical
    // behavior are not defined yet. See CONTEXT.md > Open Questions.
    Logger::info("Hardware layer initialized (stub, GPIO mapping not defined)");
}

void initializeNetwork() {
    // Wi-Fi credentials/provisioning mechanism not defined yet, so we do
    // not call wifiManager.begin() with real credentials here.
    // See CONTEXT.md > Open Questions.
    Logger::info("Network layer initialized (WiFi not yet provisioned)");
}

void initializeIntercom() {
    Logger::info("Intercom layer initialized");
}

void initializeStateMachine() {
    stateMachine.setTransitionCallback(onStateTransition);
    stateMachine.finishBoot();
}

void updateNetwork() {
    auto event = wifiManager.update();
    if (event) {
        Logger::event(toString(event->type));
        stateMachine.handleEvent(*event);
    }
    protocol.update();
}

void updateIntercom() {
    auto event = intercom.update();
    if (event) {
        Logger::event(toString(event->type));
        stateMachine.handleEvent(*event);
    }
}

void updateStateMachine() {
    // Currently a no-op: all transitions today are event-driven (see
    // updateNetwork/updateIntercom). Reserved for future timeout/retry
    // logic (e.g. a ringing timeout, error recovery). See CONTEXT.md.
}

} // namespace

void setup() {
    initializeLogging();
    initializeHardware();
    initializeNetwork();
    initializeIntercom();
    initializeStateMachine();
}

void loop() {
    updateNetwork();
    updateIntercom();
    updateStateMachine();
}
