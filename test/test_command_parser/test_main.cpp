#include <string>

#include <unity.h>

#include "../../src/protocol/messages.h"

using namespace interbridge;

namespace {
// Canonical 32-lowercase-hex command_id format, per
// docs/communication-protocol.md > Common Message Fields.
constexpr const char* kValidCommandId = "0123456789abcdef0123456789abcdef";
} // namespace

void setUp() {}
void tearDown() {}

void test_valid_command_with_unix_timestamps_parses_ok() {
    auto result = parseCommand(
        R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":")" + std::string(kValidCommandId) +
        R"(","issued_at":1786467600,"expires_at":1786467610})");

    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandType::OpenDoor), static_cast<int>(result.command.type));
    TEST_ASSERT_EQUAL_STRING(kValidCommandId, result.command.commandId.c_str());
    TEST_ASSERT_TRUE(result.command.hasIssuedAt);
    TEST_ASSERT_TRUE(result.command.hasExpiresAt);
    TEST_ASSERT_EQUAL(1786467600, result.command.issuedAtUnixSeconds);
    TEST_ASSERT_EQUAL(1786467610, result.command.expiresAtUnixSeconds);
}

void test_iso8601_string_timestamp_is_not_accepted_as_a_command_timestamp() {
    // Command issued_at/expires_at must be Unix epoch seconds (integers),
    // never ISO-8601 strings - unlike DeviceEvent::timestamp, which is
    // ISO-8601. An ISO-8601 string here is treated as if the field were
    // absent, not parsed as a timestamp.
    auto result = parseCommand(
        R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":")" + std::string(kValidCommandId) +
        R"(","issued_at":"2026-08-11T14:30:25Z","expires_at":"2026-08-11T14:30:35Z"})");

    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_FALSE(result.command.hasIssuedAt);
    TEST_ASSERT_FALSE(result.command.hasExpiresAt);
}

void test_malformed_json_is_invalid_payload() {
    auto result = parseCommand(R"({"protocol_version":1,"command":)");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_missing_command_id_is_invalid_payload() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"OPEN_DOOR"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_invalid_command_id_format_is_rejected() {
    // Not 32 lowercase hex characters (has a prefix and is too short).
    auto result = parseCommand(R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":"cmd-1"})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_uppercase_command_id_is_rejected() {
    std::string uppercase = "0123456789ABCDEF0123456789ABCDEF";
    auto result =
        parseCommand(R"({"protocol_version":1,"command":"OPEN_DOOR","command_id":")" + uppercase + R"("})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_missing_protocol_version_is_invalid_payload() {
    auto result =
        parseCommand(R"({"command":"OPEN_DOOR","command_id":")" + std::string(kValidCommandId) + R"("})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::InvalidPayload), static_cast<int>(result.status));
}

void test_unsupported_protocol_version_is_rejected() {
    auto result = parseCommand(R"({"protocol_version":2,"command":"OPEN_DOOR","command_id":")" +
                                std::string(kValidCommandId) + R"("})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::UnsupportedProtocolVersion), static_cast<int>(result.status));
}

void test_unknown_command_still_parses_ok_with_unknown_type() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"DO_A_BARREL_ROLL","command_id":")" +
                                std::string(kValidCommandId) + R"("})");
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
    auto result = parseCommand(R"({"protocol_version":1,"command":"UPDATE_FIRMWARE","command_id":")" +
                                std::string(kValidCommandId) + R"(","payload":{"version":"0.2.0"}})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_TRUE(result.command.rawPayload.find("0.2.0") != std::string::npos);
}

void test_reserved_call_commands_recognized() {
    auto result = parseCommand(R"({"protocol_version":1,"command":"ANSWER_CALL","command_id":")" +
                                std::string(kValidCommandId) + R"("})");
    TEST_ASSERT_EQUAL(static_cast<int>(CommandParseStatus::Ok), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandType::AnswerCall), static_cast<int>(result.command.type));
}

void test_is_valid_command_id() {
    TEST_ASSERT_TRUE(isValidCommandId("0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_FALSE(isValidCommandId(""));
    TEST_ASSERT_FALSE(isValidCommandId("cmd-1"));
    TEST_ASSERT_FALSE(isValidCommandId("0123456789ABCDEF0123456789abcdef")); // uppercase
    TEST_ASSERT_FALSE(isValidCommandId("0123456789abcdef0123456789abcde"));  // 31 chars
    TEST_ASSERT_FALSE(isValidCommandId("0123456789abcdef0123456789abcdef0")); // 33 chars
}

void test_all_protocol_error_codes_have_stable_strings() {
    TEST_ASSERT_EQUAL_STRING("INVALID_PAYLOAD", toString(ProtocolErrorCode::InvalidPayload));
    TEST_ASSERT_EQUAL_STRING("PAYLOAD_TOO_LARGE", toString(ProtocolErrorCode::PayloadTooLarge));
    TEST_ASSERT_EQUAL_STRING("UNSUPPORTED_PROTOCOL_VERSION", toString(ProtocolErrorCode::UnsupportedProtocolVersion));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN_COMMAND", toString(ProtocolErrorCode::UnknownCommand));
    TEST_ASSERT_EQUAL_STRING("COMMAND_NOT_ALLOWED", toString(ProtocolErrorCode::CommandNotAllowed));
    TEST_ASSERT_EQUAL_STRING("COMMAND_EXPIRED", toString(ProtocolErrorCode::CommandExpired));
    TEST_ASSERT_EQUAL_STRING("CLOCK_NOT_TRUSTWORTHY", toString(ProtocolErrorCode::ClockNotTrustworthy));
    TEST_ASSERT_EQUAL_STRING("INVALID_TIMESTAMP", toString(ProtocolErrorCode::InvalidTimestamp));
    TEST_ASSERT_EQUAL_STRING("DEVICE_BUSY", toString(ProtocolErrorCode::DeviceBusy));
    TEST_ASSERT_EQUAL_STRING("NOT_PROVISIONED", toString(ProtocolErrorCode::NotProvisioned));
    TEST_ASSERT_EQUAL_STRING("WIFI_UNAVAILABLE", toString(ProtocolErrorCode::WifiUnavailable));
    TEST_ASSERT_EQUAL_STRING("CLOUD_UNAVAILABLE", toString(ProtocolErrorCode::CloudUnavailable));
    TEST_ASSERT_EQUAL_STRING("DOOR_OUTPUT_FAILURE", toString(ProtocolErrorCode::DoorOutputFailure));
    TEST_ASSERT_EQUAL_STRING("OTA_DOWNLOAD_FAILED", toString(ProtocolErrorCode::OtaDownloadFailed));
    TEST_ASSERT_EQUAL_STRING("OTA_VALIDATION_FAILED", toString(ProtocolErrorCode::OtaValidationFailed));
    TEST_ASSERT_EQUAL_STRING("OTA_INSTALL_FAILED", toString(ProtocolErrorCode::OtaInstallFailed));
    TEST_ASSERT_EQUAL_STRING("PROVISIONING_FAILED", toString(ProtocolErrorCode::ProvisioningFailed));
    TEST_ASSERT_EQUAL_STRING("INTERNAL_ERROR", toString(ProtocolErrorCode::InternalError));
}

void test_all_protocol_error_codes_have_a_default_message() {
    // Every code must produce a non-empty, stable message (used both when
    // first raised and when a duplicate command response is replayed
    // from the dedup cache - see command_cache.h).
    ProtocolErrorCode codes[] = {
        ProtocolErrorCode::InvalidPayload,       ProtocolErrorCode::PayloadTooLarge,
        ProtocolErrorCode::UnsupportedProtocolVersion, ProtocolErrorCode::UnknownCommand,
        ProtocolErrorCode::CommandNotAllowed,     ProtocolErrorCode::CommandExpired,
        ProtocolErrorCode::ClockNotTrustworthy,    ProtocolErrorCode::InvalidTimestamp,
        ProtocolErrorCode::DeviceBusy,              ProtocolErrorCode::NotProvisioned,
        ProtocolErrorCode::WifiUnavailable,          ProtocolErrorCode::CloudUnavailable,
        ProtocolErrorCode::DoorOutputFailure,         ProtocolErrorCode::OtaDownloadFailed,
        ProtocolErrorCode::OtaValidationFailed,        ProtocolErrorCode::OtaInstallFailed,
        ProtocolErrorCode::ProvisioningFailed,          ProtocolErrorCode::InternalError,
    };
    for (auto code : codes) {
        TEST_ASSERT_FALSE(defaultErrorMessage(code).empty());
    }
}

void test_canonical_intercom_states() {
    TEST_ASSERT_EQUAL_STRING("IDLE", toString(ProtocolIntercomState::Idle));
    TEST_ASSERT_EQUAL_STRING("RINGING", toString(ProtocolIntercomState::Ringing));
    TEST_ASSERT_EQUAL_STRING("OFF_HOOK", toString(ProtocolIntercomState::OffHook));
    TEST_ASSERT_EQUAL_STRING("IN_CALL", toString(ProtocolIntercomState::InCall));
    TEST_ASSERT_EQUAL_STRING("ERROR", toString(ProtocolIntercomState::Error));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_valid_command_with_unix_timestamps_parses_ok);
    RUN_TEST(test_iso8601_string_timestamp_is_not_accepted_as_a_command_timestamp);
    RUN_TEST(test_malformed_json_is_invalid_payload);
    RUN_TEST(test_missing_command_id_is_invalid_payload);
    RUN_TEST(test_invalid_command_id_format_is_rejected);
    RUN_TEST(test_uppercase_command_id_is_rejected);
    RUN_TEST(test_missing_protocol_version_is_invalid_payload);
    RUN_TEST(test_unsupported_protocol_version_is_rejected);
    RUN_TEST(test_unknown_command_still_parses_ok_with_unknown_type);
    RUN_TEST(test_payload_too_large_is_rejected);
    RUN_TEST(test_payload_object_is_captured_as_raw_json);
    RUN_TEST(test_reserved_call_commands_recognized);
    RUN_TEST(test_is_valid_command_id);
    RUN_TEST(test_all_protocol_error_codes_have_stable_strings);
    RUN_TEST(test_all_protocol_error_codes_have_a_default_message);
    RUN_TEST(test_canonical_intercom_states);
    return UNITY_END();
}
