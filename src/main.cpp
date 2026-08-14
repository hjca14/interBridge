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

void handleIncomingCommand(const std::string& topic, const std::string& payload) {
    (void)topic;
    auto parsed = parseCommand(payload);

    CommandResponse response;
    if (parsed.status != CommandParseStatus::Ok) {
        ProtocolErrorCode code = ProtocolErrorCode::InvalidPayload;
        if (parsed.status == CommandParseStatus::PayloadTooLarge) {
            code = ProtocolErrorCode::PayloadTooLarge;
        } else if (parsed.status == CommandParseStatus::UnsupportedProtocolVersion) {
            code = ProtocolErrorCode::UnsupportedProtocolVersion;
        }
        response.deviceId = deviceIdentity.deviceId;
        response.status = CommandStatus::Rejected;
        response.error = ProtocolError{code, defaultErrorMessage(code)};
    } else {
        response = commandHandler->handle(parsed.command);
    }

    transport.publish(mqttTopics->responsesIngest(), response.toJson(), MqttQos::AtLeastOnce);
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

void initializeNetwork() {
    Logger::info("Network layer initialized (AWS IoT MQTT/TLS transport not implemented yet)");
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

    if (!subscribedToCommands) {
        subscribedToCommands = transport.subscribe(mqttTopics->commands(), handleIncomingCommand);
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

    // AWS IoT Jobs / OTA: currently always a no-op because
    // Esp32JobsClient and Esp32AwsIotTransport are stubs.
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
