#include <unity.h>

#include "../../src/protocol/event_outbox.h"
#include "../../src/storage/memory_store.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_enqueue_then_pending_returns_entry() {
    MemoryEventOutbox outbox;
    outbox.enqueue("evt-1", R"({"event":"OFF_HOOK"})");

    auto pending = outbox.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pending.size()));
    TEST_ASSERT_EQUAL_STRING("evt-1", pending[0].eventId.c_str());
}

void test_dequeue_removes_entry() {
    MemoryEventOutbox outbox;
    outbox.enqueue("evt-1", "{}");
    outbox.dequeue("evt-1");

    TEST_ASSERT_EQUAL(0, static_cast<int>(outbox.size()));
}

void test_capacity_evicts_oldest_first() {
    MemoryEventOutbox outbox(2);
    outbox.enqueue("evt-1", "{}");
    outbox.enqueue("evt-2", "{}");
    outbox.enqueue("evt-3", "{}");

    auto pending = outbox.pending();
    TEST_ASSERT_EQUAL(2, static_cast<int>(pending.size()));
    TEST_ASSERT_EQUAL_STRING("evt-2", pending[0].eventId.c_str());
    TEST_ASSERT_EQUAL_STRING("evt-3", pending[1].eventId.c_str());
}

void test_enqueue_same_id_twice_does_not_duplicate() {
    MemoryEventOutbox outbox;
    outbox.enqueue("evt-1", "{\"v\":1}");
    outbox.enqueue("evt-1", "{\"v\":2}");

    TEST_ASSERT_EQUAL(1, static_cast<int>(outbox.size()));
    TEST_ASSERT_TRUE(outbox.pending()[0].eventJson.find("2") != std::string::npos);
}

void test_persistent_outbox_survives_reload_preserving_event_id() {
    MemoryStore store;
    {
        PersistentEventOutbox outbox(store);
        outbox.enqueue("evt-stable-id", R"({"event":"RING_DETECTED"})");
    }

    PersistentEventOutbox reloaded(store);
    auto pending = reloaded.pending();
    TEST_ASSERT_EQUAL(1, static_cast<int>(pending.size()));
    TEST_ASSERT_EQUAL_STRING("evt-stable-id", pending[0].eventId.c_str());
}

void test_persistent_outbox_dequeue_persists_across_reload() {
    MemoryStore store;
    {
        PersistentEventOutbox outbox(store);
        outbox.enqueue("evt-1", "{}");
        outbox.enqueue("evt-2", "{}");
        outbox.dequeue("evt-1");
    }

    PersistentEventOutbox reloaded(store);
    TEST_ASSERT_EQUAL(1, static_cast<int>(reloaded.size()));
    TEST_ASSERT_EQUAL_STRING("evt-2", reloaded.pending()[0].eventId.c_str());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_enqueue_then_pending_returns_entry);
    RUN_TEST(test_dequeue_removes_entry);
    RUN_TEST(test_capacity_evicts_oldest_first);
    RUN_TEST(test_enqueue_same_id_twice_does_not_duplicate);
    RUN_TEST(test_persistent_outbox_survives_reload_preserving_event_id);
    RUN_TEST(test_persistent_outbox_dequeue_persists_across_reload);
    return UNITY_END();
}
