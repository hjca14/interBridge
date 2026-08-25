#include <Arduino.h>

#include <optional>
#include <string>

#include "audio/audio.h"
#include "aws/device_shadow.h"
#include "aws/jobs.h"
#include "core/events.h"
#include "core/logger.h"
#include "core/random_id.h"
#include "core/state_machine.h"
#include "core/version.h"
#include "hardware/button.h"
#include "hardware/clock.h"
#include "hardware/gpio.h"
#include "hardware/status_indicator.h"
#include "hardware/system_control.h"
#include "intercom/intercom.h"
#include "intercom/si3050/si3050_bus.h"
#include "intercom/si3050/si3050_controller.h"
#include "intercom/si3050/si3050_delay.h"
#include "intercom/si3050/si3050_pcm_clock.h"
#include "intercom/si3050/si3050_reset.h"
#include "network/health_reporter.h"
#include "network/mqtt_topics.h"
#include "network/mqtt_transport.h"
#include "network/reconnect_manager.h"
#include "network/wifi.h"
#include "ota/firmware_validation.h"
#include "ota/ota_manager.h"
#include "protocol/command_cache.h"
#include "protocol/command_handler.h"
#include "protocol/event_outbox.h"
#include "protocol/messages.h"
#include "protocol/remote_command_processor.h"
#include "provisioning/ble_provisioning.h"
#include "provisioning/device_identity.h"
#include "provisioning/factory_reset_coordinator.h"
#include "provisioning/fleet_provisioning.h"
#include "provisioning/provisioning_manager.h"
#include "storage/credential_store.h"
#include "storage/nvs_store.h"

using namespace interbridge;

namespace {

// Provisional - see CONTEXT.md > Open Questions (exact board not final).
constexpr const char* kHardwareVersion = "1.0";

// ---- Core / hardware ----
Esp32GpioHardware hardware;
Intercom intercom(hardware);
StateMachine stateMachine;
NullAudioIO audio; // Placeholder until audio hardware is defined.
Esp32Clock clock;
Esp32RandomSource randomSource;
Esp32SystemControl systemControl;
Esp32ButtonInput buttonInputHardware;
ButtonController buttonController(buttonInputHardware);
Esp32StatusIndicator statusIndicator;

// ---- Si3050 PCM clock bring-up ----
// Constructed inside setup() (initializeSi3050()), not at global scope
// like the hardware objects above: Esp32Si3050Bus's/Esp32Si3050Reset's
// constructors call real pinMode()/digitalWrite() under #ifdef ARDUINO
// (see si3050_bus.cpp/si3050_reset.cpp), which must not run before the
// Arduino runtime itself has initialized - no other object in this file
// touches a real pin from a global constructor, for the same reason.
// See CONTEXT.md and docs/si3050-bringup.md for what this does and does
// not validate: the PCLK/FSYNC clock geometry is physically validated
// (docs/si3050-clock-probe.md), but Esp32Si3050Bus::transfer() (real SPI)
// remains an unimplemented TODO, so this integrates clock generation and
// its bring-up ordering only - no DAA/register configuration, ring,
// off-hook, or audio.
std::optional<Esp32Si3050Bus> si3050Bus;
std::optional<Esp32PcmClock> si3050PcmClock;
std::optional<Esp32Si3050Reset> si3050Reset;
std::optional<Esp32Si3050Delay> si3050Delay;
std::optional<Si3050Controller> si3050Controller;

// ---- Storage / identity ----
NvsStore persistentStore; // STUB - see CONTEXT.md, nothing survives reboot yet.
DeviceCredentialStore credentialStore(persistentStore);
DeviceIdentityProvider identityProvider(persistentStore, randomSource, kHardwareVersion, FIRMWARE_VERSION);
DeviceIdentity deviceIdentity;

// ---- Network ----
WifiManager wifiManager;
AwsIotConnectionConfig awsConfig; // empty placeholders - see CONTEXT.md > Open Questions.
Esp32AwsIotTransport transport(awsConfig, credentialStore);
ReconnectManager reconnectManager(randomSource);
HealthReporter healthReporter;
std::optional<MqttTopics> mqttTopics;
bool subscribedToCommands = false;
uint32_t nextConnectAttemptAtMs = 0;

// ---- Protocol ----
PersistentDedupCache dedupCache(persistentStore);
PersistentEventOutbox eventOutbox(persistentStore);
std::optional<CommandHandler> commandHandler;
std::optional<RemoteCommandProcessor> remoteCommandProcessor;

// ---- Provisioning ----
Esp32BleProvisioning bleProvisioning; // default Security2-requested, see ble_provisioning.h
FactoryResetCoordinator factoryResetCoordinator(persistentStore);
Esp32KeyPairGenerator keyPairGenerator;
Esp32FleetProvisioningTransport fleetProvisioningTransport;
// Constructed (emplaced) in initializeIdentity(), before provisioningManager
// since ProvisioningManager holds a reference to it - now genuinely
// invoked as part of the onboarding flow (ProvisioningManager::
// advanceAfterWifiConnected()), unlike the prior pass where this existed
// but nothing called it.
std::optional<FleetProvisioningCoordinator> fleetProvisioningCoordinator;
std::optional<ProvisioningManager> provisioningManager;

// ---- OTA / AWS IoT Jobs ----
DefaultFirmwareVerifier firmwareVerifier;
Esp32OtaPlatform otaPlatform;
OtaCoordinator otaCoordinator(otaPlatform, firmwareVerifier, FIRMWARE_VERSION);
Esp32JobsClient jobsClient;
JobsCoordinator jobsCoordinator(jobsClient, otaCoordinator);

void onStateTransition(State from, State to) {
    Logger::stateTransition(toString(from), toString(to));
}

// Maps a core (internal) event to the public protocol event vocabulary.
// Only events the firmware genuinely produces map to something -
// DoorOpenRequested has no matching protocol event (DOOR_OPENED/
// DOOR_OPEN_FAILED are produced directly by CommandHandler, from the
// result of actuation, not the request), and Wi-Fi connectivity is
// carried through Device Shadow/health reports rather than a discrete
// event in this protocol version. See CONTEXT.md > Decisions.
std::optional<ProtocolEventName> toProtocolEvent(EventType type) {
    switch (type) {
        case EventType::RingDetected: return ProtocolEventName::RingDetected;
        case EventType::OffHook: return ProtocolEventName::OffHook;
        case EventType::OnHook: return ProtocolEventName::OnHook;
        case EventType::CallStarted: return ProtocolEventName::CallStarted;
        case EventType::CallEnded: return ProtocolEventName::CallEnded;
        case EventType::DoorOpenRequested: return std::nullopt;
        case EventType::WifiConnected: return std::nullopt;
        case EventType::WifiDisconnected: return std::nullopt;
    }
    return std::nullopt;
}

// Maps the core state machine's internal State to the protocol's closed
// intercom_state vocabulary (see protocol/messages.h ::ProtocolIntercomState).
// State::Boot has no protocol equivalent; it is mapped defensively to
// Idle rather than left unhandled, but this branch should be unreachable
// in practice since finishBoot() always runs (in initializeStateMachine())
// before loop() - and therefore before anything that publishes
// intercom_state - ever executes. State::InCall always maps to IN_CALL:
// the state machine transitions directly from Ringing to InCall on the
// OffHook event, so there is no distinct resting "off-hook" state to map
// ProtocolIntercomState::OffHook from - see messages.h and CONTEXT.md.
ProtocolIntercomState toProtocolIntercomState(State state) {
    switch (state) {
        case State::Boot: return ProtocolIntercomState::Idle;
        case State::Idle: return ProtocolIntercomState::Idle;
        case State::Ringing: return ProtocolIntercomState::Ringing;
        case State::InCall: return ProtocolIntercomState::InCall;
        case State::Error: return ProtocolIntercomState::Error;
    }
    return ProtocolIntercomState::Error;
}

void publishProtocolEvent(ProtocolEventName eventName) {
    DeviceEvent event;
    event.deviceId = deviceIdentity.deviceId;
    event.event = eventName;
    event.eventId = generateHexId(randomSource, "evt");
    // event.timestamp intentionally left empty: no NTP/time sync yet
    // (see hardware/clock.h) - the protocol requires not inventing a
    // timestamp when wall-clock time isn't trustworthy.
    eventOutbox.enqueue(event.eventId, event.toJson());
}

void initializeLogging() {
    Serial.begin(115200);
    Logger::info("Booting InterBridge");
    Logger::info("Firmware version: " FIRMWARE_VERSION);
}

void initializeIdentity() {
    deviceIdentity = identityProvider.load();
    Logger::info(("Device ID: " + deviceIdentity.deviceId).c_str());
    // setup_code is human-facing (QR/manual fallback) and low-stakes
    // compared to device_id/credentials - still avoided in routine logs
    // as a matter of habit, not because it's cryptographically sensitive.

    MqttTopicsConfig topicsConfig;
    topicsConfig.deviceId = deviceIdentity.deviceId;
    // Production rule names remain intentionally unconfigured. The separate
    // DEV smoke entry point supplies its backend-matched names explicitly.
    mqttTopics.emplace(topicsConfig);

    commandHandler.emplace(deviceIdentity.deviceId, clock, dedupCache, intercom, systemControl);
    remoteCommandProcessor.emplace(deviceIdentity.deviceId, transport, *commandHandler, *mqttTopics);

    fleetProvisioningCoordinator.emplace(keyPairGenerator, fleetProvisioningTransport, credentialStore,
                                          topicsConfig.fleetProvisioningTemplateName);

    // Provisional Proof-of-Possession: regenerated every boot rather than
    // persisted, since the production PoP strategy is not decided yet.
    // See CONTEXT.md > Open Questions.
    std::string proofOfPossession = generateHexId(randomSource, "pop");
    BleAdvertisementInfo advertisementInfo = buildBleAdvertisementInfo(deviceIdentity.deviceId);
    provisioningManager.emplace(persistentStore, wifiManager, bleProvisioning, credentialStore,
                                 *fleetProvisioningCoordinator, statusIndicator, deviceIdentity.deviceId,
                                 proofOfPossession, advertisementInfo);
    provisioningManager->checkAtBoot(clock.monotonicMs());
}

void initializeHardware() {
    // Esp32GpioHardware is currently a stub: GPIO mapping and electrical
    // behavior are not defined yet. See CONTEXT.md > Open Questions.
    Logger::info("Hardware layer initialized (stub, GPIO mapping not defined)");
}

void initializeSi3050() {
    // Runs the Si3050's documented electrical bring-up sequence (CS
    // deselected, /RESET asserted, SCLK held high, PCLK/FSYNC started,
    // then /RESET released only once the clock is confirmed running -
    // see Si3050Controller). The PCM clock geometry (16 x 8 TDM, PCM
    // short, 1.024 MHz PCLK / 8 kHz FSYNC) is physically validated on
    // real ESP32-C3 hardware - see docs/si3050-clock-probe.md. This call
    // only brings up the clock and its electrical ordering: it never
    // reads or writes a single Si3050 control register (DAA/line/audio
    // configuration remains out of scope - see Si3050Controller and
    // docs/si3050-bringup.md), and Esp32Si3050Bus::transfer() (real SPI)
    // is still an unimplemented TODO, so isReady()==true here does not
    // mean a real Si3050 has been read from. Safe to run with no Si3050
    // physically attached: /RESET is simply released on an otherwise
    // idle GPIO, and si3050_pins.h's GPIO0/GPIO1 (PCLK/FSYNC) are the
    // same pins already used - and physically validated - by the Phase
    // 3B.1 clock probe.
    si3050Bus.emplace();
    si3050PcmClock.emplace();
    si3050Reset.emplace();
    si3050Delay.emplace();
    si3050Controller.emplace(*si3050Bus, *si3050PcmClock, *si3050Reset, *si3050Delay);

    Si3050InitResult result = si3050Controller->initialize();
    switch (result) {
        case Si3050InitResult::Ready:
            Logger::info("Si3050 PCM clock started (1.024 MHz PCLK / 8 kHz FSYNC, PCM/SPI bring-up complete)");
            break;
        case Si3050InitResult::ClockNotRunning:
            Logger::error("Si3050 PCM clock failed to start - bring-up stopped, /RESET stays asserted");
            break;
        case Si3050InitResult::InvalidConfig:
            Logger::error("Si3050: invalid Si3050Config (pclkHz/fsyncHz == 0) - bring-up skipped");
            break;
    }
}

void initializeNetwork() {
    Logger::info("Network layer initialized (AWS IoT MQTT/mTLS; fail-closed until configured)");
}

void initializeIntercom() {
    Logger::info("Intercom layer initialized");
}

void initializeStateMachine() {
    stateMachine.setTransitionCallback(onStateTransition);
    stateMachine.finishBoot();
}

void updateButton() {
    uint32_t nowMs = clock.monotonicMs();
    ButtonAction action = buttonController.update(nowMs);
    if (action == ButtonAction::ProvisioningRequested) {
        provisioningManager->requestProvisioning(nowMs);
    } else if (action == ButtonAction::FactoryResetRequested) {
        statusIndicator.show(ProvisioningIndication::FactoryResetWarning);
        factoryResetCoordinator.execute();
        publishProtocolEvent(ProtocolEventName::FactoryResetRequested);
        systemControl.restart();
    }
}

void updateProvisioning() {
    provisioningManager->update(clock.monotonicMs());
    auto event = provisioningManager->pollEvent();
    if (event.has_value()) {
        publishProtocolEvent(*event);
    }
}

void updateIntercom() {
    auto event = intercom.update();
    if (event) {
        Logger::event(toString(event->type));
        stateMachine.handleEvent(*event);

        auto protocolEvent = toProtocolEvent(event->type);
        if (protocolEvent.has_value()) {
            publishProtocolEvent(*protocolEvent);
        }
    }
}

void updateNetwork() {
    auto wifiEvent = wifiManager.update();
    if (wifiEvent) {
        Logger::event(toString(wifiEvent->type));
        stateMachine.handleEvent(*wifiEvent);
    }

    if (!wifiManager.isConnected()) {
        if (transport.isConnected()) {
            transport.disconnect();
            subscribedToCommands = false;
        }
        return; // nothing more to do without a network link
    }

    if (!transport.isConnected()) {
        uint32_t now = clock.monotonicMs();
        if (now < nextConnectAttemptAtMs) {
            return; // backing off, see ReconnectManager
        }

        if (transport.connect(MqttTopics::clientId(deviceIdentity.deviceId))) {
            reconnectManager.reset();
            subscribedToCommands = false;
            healthReporter.forceNextPublish(); // publish health/Shadow right after reconnect
        } else {
            nextConnectAttemptAtMs = now + reconnectManager.nextDelayMs();
            return;
        }
    }

    transport.poll();
    remoteCommandProcessor->processPending();

    if (!subscribedToCommands) {
        subscribedToCommands = remoteCommandProcessor->subscribe();
    }

    for (const auto& entry : eventOutbox.pending()) {
        if (transport.publish(mqttTopics->eventsIngest(), entry.eventJson, MqttQos::AtLeastOnce)) {
            eventOutbox.dequeue(entry.eventId);
        }
    }

    if (healthReporter.isDue(clock.monotonicMs())) {
        HealthReport health;
        health.deviceId = deviceIdentity.deviceId;
        health.firmwareVersion = FIRMWARE_VERSION;
        health.intercomState = toString(toProtocolIntercomState(stateMachine.getState()));
        health.uptimeMs = clock.monotonicMs();
        health.wifiRssi = 0;      // TODO: RSSI not wired up yet, see CONTEXT.md
        health.freeHeapBytes = 0; // TODO: not wired up yet, see CONTEXT.md
        transport.publish(mqttTopics->healthIngest(), health.toJson(), MqttQos::AtMostOnce);

        ShadowReportedState shadowState;
        shadowState.firmwareVersion = FIRMWARE_VERSION;
        shadowState.hardwareVersion = kHardwareVersion;
        shadowState.intercomState = health.intercomState;
        shadowState.wifiRssi = health.wifiRssi;
        shadowState.uptimeMs = health.uptimeMs;
        shadowState.provisioned = deviceIdentity.provisioned;
        transport.publish(mqttTopics->shadowUpdate(), DeviceShadow::buildReportedUpdate(shadowState),
                           MqttQos::AtLeastOnce);
    }

    // AWS IoT Jobs / OTA remains a no-op because Esp32JobsClient is a stub.
    auto otaResult = jobsCoordinator.pollAndProcess();
    if (otaResult.has_value()) {
        publishProtocolEvent(*otaResult == OtaResult::Success ? ProtocolEventName::OtaCompleted
                                                                : ProtocolEventName::OtaFailed);
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
    initializeIdentity();
    initializeHardware();
    initializeSi3050();
    initializeNetwork();
    initializeIntercom();
    initializeStateMachine();
}

void loop() {
    updateButton();
    updateProvisioning();
    updateNetwork();
    updateIntercom();
    updateStateMachine();
}
