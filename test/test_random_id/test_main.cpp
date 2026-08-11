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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_generated_id_has_expected_format);
    RUN_TEST(test_same_seed_produces_same_id);
    RUN_TEST(test_consecutive_ids_from_same_source_differ);
    return UNITY_END();
}
