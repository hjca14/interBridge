#include <unity.h>

#include "../../src/hardware/clock.h"
#include "../../src/hardware/gpio.h"
#include "../../src/hardware/system_control.h"
#include "../../src/intercom/intercom.h"
#include "../../src/protocol/command_cache.h"
#include "../../src/protocol/command_handler.h"

using namespace interbridge;

namespace {

class MockHardware : public IHardwareIO {
public:
    bool line = false;
    bool doorOutputSucceeds = true;
    int doorCallCount = 0;

    bool readLineState() override { return line; }
    bool setDoorOutput(bool enabled) override {
        if (enabled) doorCallCount++;
        return doorOutputSucceeds;
    }
};

DeviceCommand makeCommand(CommandType type, const std::string& rawCommand, const std::string& id,
                           int64_t issuedAt, int64_t expiresAt) {
    DeviceCommand cmd;
    cmd.type = type;
    cmd.rawCommand = rawCommand;
    cmd.commandId = id;
    cmd.issuedAtUnixSeconds = issuedAt;
    cmd.hasIssuedAt = true;
    cmd.expiresAtUnixSeconds = expiresAt;
    cmd.hasExpiresAt = true;
    return cmd;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_open_door_is_accepted_then_rejected_when_capability_disabled() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-1", 995, 1005);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_TRUE(response.hasAccepted);
    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Accepted), static_cast<int>(response.accepted.status));
    TEST_ASSERT_TRUE(response.terminal.error.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CapabilityDisabled),
                      static_cast<int>(response.terminal.error->code));
    TEST_ASSERT_EQUAL(0, hw.doorCallCount);
}

void test_open_door_does_not_call_failing_hardware_when_capability_disabled() {
    MockHardware hw;
    hw.doorOutputSucceeds = false;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-1", 995, 1005);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_TRUE(response.terminal.error.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CapabilityDisabled), static_cast<int>(response.terminal.error->code));
}

void test_open_door_rejected_when_clock_not_trustworthy() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock; // hasValidTime() is false by default
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-1", 995, 1005);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::ClockNotTrustworthy), static_cast<int>(response.terminal.error->code));
    TEST_ASSERT_EQUAL(0, hw.doorCallCount);
}

void test_open_door_rejected_when_expired() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(2000); // well past expires_at
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-1", 995, 1005);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CommandExpired), static_cast<int>(response.terminal.error->code));
    TEST_ASSERT_EQUAL(0, hw.doorCallCount);
}

void test_open_door_rejected_when_validity_window_exceeds_maximum() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    // OPEN_DOOR max validity is 30s; this window is 100s.
    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-1", 995, 1095);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::InvalidTimestamp), static_cast<int>(response.terminal.error->code));
}

void test_open_door_accepts_exact_thirty_second_window() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto command = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "test_open_door_accepts_exact_thirty_second_window", 1000, 1030);
    auto response = handler.handle(command);

    TEST_ASSERT_TRUE(response.hasAccepted);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CapabilityDisabled),
                      static_cast<int>(response.terminal.error->code));
}

void test_open_door_rejects_thirty_one_second_window() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto command = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "test_open_door_rejects_thirty_one_second_window", 1000, 1031);
    auto response = handler.handle(command);

    TEST_ASSERT_FALSE(response.hasAccepted);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::InvalidTimestamp),
                      static_cast<int>(response.terminal.error->code));
}

void test_open_door_accepts_issued_at_five_seconds_in_future() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto command = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "test_open_door_accepts_issued_at_five_seconds_in_future", 1005, 1030);
    auto response = handler.handle(command);

    TEST_ASSERT_TRUE(response.hasAccepted);
}

void test_open_door_rejects_issued_at_six_seconds_in_future() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto command = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "test_open_door_rejects_issued_at_six_seconds_in_future", 1006, 1030);
    auto response = handler.handle(command);

    TEST_ASSERT_FALSE(response.hasAccepted);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::InvalidTimestamp),
                      static_cast<int>(response.terminal.error->code));
}

void test_restart_is_rejected_without_triggering_system_control() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::Restart, "RESTART", "cmd-1", 995, 1030);
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CommandNotAllowed),
                      static_cast<int>(response.terminal.error->code));
    TEST_ASSERT_EQUAL(0, sysControl.restartCount());
}

void test_duplicate_open_door_does_not_actuate_hardware_twice() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    clock.setUnixTimeSeconds(1000);
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    auto cmd = makeCommand(CommandType::OpenDoor, "OPEN_DOOR", "cmd-dup", 995, 1005);
    auto first = handler.handle(cmd);
    auto second = handler.handle(cmd); // simulates MQTT QoS 1 redelivery

    TEST_ASSERT_EQUAL(0, hw.doorCallCount);
    TEST_ASSERT_TRUE(first.hasAccepted);
    TEST_ASSERT_FALSE(second.hasAccepted);
    TEST_ASSERT_EQUAL(static_cast<int>(first.terminal.status), static_cast<int>(second.terminal.status));
}

void test_enter_provisioning_is_not_remotely_allowed() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    DeviceCommand cmd;
    cmd.type = CommandType::EnterProvisioning;
    cmd.rawCommand = "ENTER_PROVISIONING";
    cmd.commandId = "cmd-1";
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CommandNotAllowed), static_cast<int>(response.terminal.error->code));
}

void test_factory_reset_is_not_remotely_allowed() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    DeviceCommand cmd;
    cmd.type = CommandType::FactoryReset;
    cmd.rawCommand = "FACTORY_RESET";
    cmd.commandId = "cmd-1";
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::CommandNotAllowed), static_cast<int>(response.terminal.error->code));
}

void test_reserved_call_command_is_rejected() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    DeviceCommand cmd;
    cmd.type = CommandType::AnswerCall;
    cmd.rawCommand = "ANSWER_CALL";
    cmd.commandId = "cmd-1";
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
}

void test_unknown_command_is_rejected() {
    MockHardware hw;
    Intercom intercom(hw);
    FakeClock clock;
    InMemoryDedupCache cache;
    FakeSystemControl sysControl;
    CommandHandler handler("ib-test", clock, cache, intercom, sysControl);

    DeviceCommand cmd;
    cmd.type = CommandType::Unknown;
    cmd.rawCommand = "DO_A_BARREL_ROLL";
    cmd.commandId = "cmd-1";
    auto response = handler.handle(cmd);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Rejected), static_cast<int>(response.terminal.status));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::UnknownCommand), static_cast<int>(response.terminal.error->code));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_open_door_is_accepted_then_rejected_when_capability_disabled);
    RUN_TEST(test_open_door_does_not_call_failing_hardware_when_capability_disabled);
    RUN_TEST(test_open_door_rejected_when_clock_not_trustworthy);
    RUN_TEST(test_open_door_rejected_when_expired);
    RUN_TEST(test_open_door_rejected_when_validity_window_exceeds_maximum);
    RUN_TEST(test_open_door_accepts_exact_thirty_second_window);
    RUN_TEST(test_open_door_rejects_thirty_one_second_window);
    RUN_TEST(test_open_door_accepts_issued_at_five_seconds_in_future);
    RUN_TEST(test_open_door_rejects_issued_at_six_seconds_in_future);
    RUN_TEST(test_restart_is_rejected_without_triggering_system_control);
    RUN_TEST(test_duplicate_open_door_does_not_actuate_hardware_twice);
    RUN_TEST(test_enter_provisioning_is_not_remotely_allowed);
    RUN_TEST(test_factory_reset_is_not_remotely_allowed);
    RUN_TEST(test_reserved_call_command_is_rejected);
    RUN_TEST(test_unknown_command_is_rejected);
    return UNITY_END();
}
