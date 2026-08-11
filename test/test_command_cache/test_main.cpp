#include <unity.h>

#include "../../src/protocol/command_cache.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_find_returns_nullopt_for_unknown_command() {
    InMemoryDedupCache cache;
    TEST_ASSERT_FALSE(cache.find("cmd-1").has_value());
}

void test_record_then_find_round_trips() {
    InMemoryDedupCache cache;
    DedupEntry entry{CommandStatus::Completed, false, ProtocolErrorCode::InternalError};
    cache.record("cmd-1", entry);

    auto found = cache.find("cmd-1");
    TEST_ASSERT_TRUE(found.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Completed), static_cast<int>(found->status));
    TEST_ASSERT_FALSE(found->hasError);
}

void test_capacity_evicts_oldest_entry() {
    InMemoryDedupCache cache(2);
    cache.record("cmd-1", DedupEntry{CommandStatus::Completed, false, ProtocolErrorCode::InternalError});
    cache.record("cmd-2", DedupEntry{CommandStatus::Completed, false, ProtocolErrorCode::InternalError});
    cache.record("cmd-3", DedupEntry{CommandStatus::Completed, false, ProtocolErrorCode::InternalError});

    TEST_ASSERT_FALSE(cache.find("cmd-1").has_value()); // evicted
    TEST_ASSERT_TRUE(cache.find("cmd-2").has_value());
    TEST_ASSERT_TRUE(cache.find("cmd-3").has_value());
}

void test_persistent_dedup_cache_survives_reload() {
    MemoryStore store;
    {
        PersistentDedupCache cache(store);
        cache.record("cmd-1", DedupEntry{CommandStatus::Failed, true, ProtocolErrorCode::DoorOutputFailure});
    }

    // A fresh PersistentDedupCache over the same store simulates a
    // reboot: the entry must still be there.
    PersistentDedupCache reloaded(store);
    auto found = reloaded.find("cmd-1");
    TEST_ASSERT_TRUE(found.has_value());
    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::Failed), static_cast<int>(found->status));
    TEST_ASSERT_TRUE(found->hasError);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolErrorCode::DoorOutputFailure), static_cast<int>(found->errorCode));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_find_returns_nullopt_for_unknown_command);
    RUN_TEST(test_record_then_find_round_trips);
    RUN_TEST(test_capacity_evicts_oldest_entry);
    RUN_TEST(test_persistent_dedup_cache_survives_reload);
    return UNITY_END();
}
