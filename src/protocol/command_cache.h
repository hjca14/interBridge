#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../storage/persistent_store.h"
#include "messages.h"

namespace interbridge {

constexpr size_t kDefaultDedupCacheCapacity = 32;

struct DedupEntry {
    CommandStatus status;
    bool hasError = false;
    ProtocolErrorCode errorCode = ProtocolErrorCode::InternalError;
};

// Bounded recent-command cache keyed by command_id, used to make command
// handling idempotent under MQTT QoS 1 at-least-once redelivery. See
// docs/communication-protocol.md > Duplicate Command Protection.
class IDedupCache {
public:
    virtual ~IDedupCache() = default;

    // Returns the previously recorded result if command_id was already
    // processed, or std::nullopt if this is the first time it's seen.
    virtual std::optional<DedupEntry> find(const std::string& commandId) = 0;

    // Records the terminal result for a command_id. Evicts the oldest
    // entry if the cache is at capacity.
    virtual void record(const std::string& commandId, const DedupEntry& entry) = 0;
};

// In-memory dedup cache. Does not survive reboot - see PersistentDedupCache.
class InMemoryDedupCache : public IDedupCache {
public:
    explicit InMemoryDedupCache(size_t capacity = kDefaultDedupCacheCapacity);

    std::optional<DedupEntry> find(const std::string& commandId) override;
    void record(const std::string& commandId, const DedupEntry& entry) override;

    // Snapshot of all entries, oldest first. Used by PersistentDedupCache
    // to serialize the cache; not part of IDedupCache.
    std::vector<std::pair<std::string, DedupEntry>> allEntries() const;

private:
    struct Row {
        std::string commandId;
        DedupEntry entry;
    };
    size_t capacity_;
    std::deque<Row> rows_;
};

// Persists the dedup cache to an IPersistentStore (as a single serialized
// JSON blob under one key) so OPEN_DOOR deduplication survives reboot,
// per docs/communication-protocol.md ("must survive reboot for at least
// the maximum command validity period"). Loads existing entries on
// construction.
class PersistentDedupCache : public IDedupCache {
public:
    explicit PersistentDedupCache(IPersistentStore& store, size_t capacity = kDefaultDedupCacheCapacity);

    std::optional<DedupEntry> find(const std::string& commandId) override;
    void record(const std::string& commandId, const DedupEntry& entry) override;

private:
    void load();
    void save() const;

    IPersistentStore& store_;
    InMemoryDedupCache memory_;
};

} // namespace interbridge
