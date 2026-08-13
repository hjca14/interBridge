#include <unity.h>

#include "../../src/core/random_id.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_generated_id_has_expected_format() {
    FakeRandomSource random(1);
    std::string id = generateHexId(random, "evt");

    TEST_ASSERT_EQUAL(36, static_cast<int>(id.size())); // "evt-" + 32 hex chars
    TEST_ASSERT_EQUAL_STRING("evt-", id.substr(0, 4).c_str());
    for (size_t i = 4; i < id.size(); i++) {
        char c = id[i];
        bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        TEST_ASSERT_TRUE(isHex);
    }
}

void test_same_seed_produces_same_id() {
    FakeRandomSource randomA(42);
    FakeRandomSource randomB(42);

    TEST_ASSERT_EQUAL_STRING(generateHexId(randomA, "cmd").c_str(), generateHexId(randomB, "cmd").c_str());
}

void test_consecutive_ids_from_same_source_differ() {
    FakeRandomSource random(1);
    std::string first = generateHexId(random, "evt");
    std::string second = generateHexId(random, "evt");

    TEST_ASSERT_TRUE(first != second);
}

void test_numeric_code_has_expected_length_and_digits_only() {
    FakeRandomSource random(1);
    std::string code = generateNumericCode(random, 12);

    TEST_ASSERT_EQUAL(12, static_cast<int>(code.size()));
    for (char c : code) {
        TEST_ASSERT_TRUE(c >= '0' && c <= '9');
    }
}

void test_numeric_code_format_display_groups_in_fours() {
    TEST_ASSERT_EQUAL_STRING("4827 1936 2051", formatNumericCodeForDisplay("482719362051").c_str());
}

void test_numeric_code_same_seed_produces_same_code() {
    FakeRandomSource randomA(7);
    FakeRandomSource randomB(7);
    TEST_ASSERT_EQUAL_STRING(generateNumericCode(randomA, 12).c_str(), generateNumericCode(randomB, 12).c_str());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_generated_id_has_expected_format);
    RUN_TEST(test_same_seed_produces_same_id);
    RUN_TEST(test_consecutive_ids_from_same_source_differ);
    RUN_TEST(test_numeric_code_has_expected_length_and_digits_only);
    RUN_TEST(test_numeric_code_format_display_groups_in_fours);
    RUN_TEST(test_numeric_code_same_seed_produces_same_code);
    return UNITY_END();
}
