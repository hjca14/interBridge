#include <string>

#include <unity.h>

#include "../../src/protocol/messages.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_valid_command_parses_ok() {
    auto result = parseCommand(
        R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":"cmd-1","issued_at":1000,"expires_at":1010})");

    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandType::OpenDoor), static_cast<int>(result.command.type));
    TEST_ASSERT_EQUAL_STRING("cmd-1", result.command.commandId.c_str());
    TEST_ASSERT_TRUE(result.command.hasIssuedAt);
    TEST_ASSERT_TRUE(result.command.hasExpiresAt);
    TEST_ASSERT_EQUAL(1000, result.command.issuedAtUnixSeconds);
    TEST_ASSERT_EQUAL(1010, result.command.expiresAtUnixSeconds);
}

void test_malformed_json_is_invalid_payload() {
    auto result = parseCommand(R"({"protocol_version":1,"command":)");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_missing_command_id_is_invalid_payload() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"OPEN_DOOR"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_missing_protocol_version_is_invalid_payload() {
    auto result = parseCommand(R"({"command":"OPEN_DOOR","command_id":"cmd-1"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_unsupported_protocol_version_is_rejected() {
    auto result = parseCommand(R"({"protocol_version":2,"command":"OPEN_DOOR","command_id":"cmd-1"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::UnsupportedProtocolVersion), static_cast<int>(result.status));
}

void test_unknown_command_still_parses_ok_with_unknown_type() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"DO_A_BARREL_ROLL","command_id":"cmd-1"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandType::Unknown), static_cast<int>(result.command.type));
    TEST_ASSERT_EQUAL_STRING("DO_A_BARREL_ROLL", result.command.rawCommand.c_str());
}

void test_payload_too_large_is_rejected() {
    std::string huge(kMaxJsonPayloadBytes + 1, 'a');
    auto result = parseCommand(huge);
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::PayloadTooLarge), static_cast<int>(result.status));
}

void test_payload_object_is_captured_as_raw_json() {
    auto result = parseCommand(
        R"({"protocol_version":1,"command":"UPDATE_FIRMWARE","command_id":"cmd-1","payload":{"version":"0.2.0"}})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_TRUE(result.command.rawPayload.find("0.2.0") != std::string::npos);
}

void test_reserved_call_commands_recognized() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"ANSWER_CALL","command_id":"cmd-1"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandType::AnswerCall), static_cast<int>(result.command.type));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_valid_command_parses_ok);
    RUN_TEST(test_malformed_json_is_invalid_payload);
    RUN_TEST(test_missing_command_id_is_invalid_payload);
    RUN_TEST(test_missing_protocol_version_is_invalid_payload);
    RUN_TEST(test_unsupported_protocol_version_is_rejected);
    RUN_TEST(test_unknown_command_still_parses_ok_with_unknown_type);
    RUN_TEST(test_payload_too_large_is_rejected);
    RUN_TEST(test_payload_object_is_captured_as_raw_json);
    RUN_TEST(test_reserved_call_commands_recognized);
    return UNITY_END();
}
