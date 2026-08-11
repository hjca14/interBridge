#include "command_cache.h"

#define ARDUINOJSON_ENABLE_STD_STRING 1
#include <ArduinoJson.h>

namespace {
constexpr const char* kStorageKey = "command_dedup_cache";
}

namespace interbridge {

InMemoryDedupCache::InMemoryDedupCache(size_t capacity) : capacity_(capacity) {}

std::optional<DedupEntry> InMemoryDedupCache::find(const std::string& commandId) {
    for (const auto& row : rows_) {
        if (row.commandId == commandId) {
            return row.entry;
        }
    }
    return std::nullopt;
}

void InMemoryDedupCache::record(const std::string& commandId, const DedupEntry& entry) {
    for (auto& row : rows_) {
        if (row.commandId == commandId) {
            row.entry = entry;
            return;
        }
    }
    if (rows_.size() >= capacity_) {
        rows_.pop_front();
    }
    rows_.push_back(Row{commandId, entry});
}

std::vector<std::pair<std::string, DedupEntry>> InMemoryDedupCache::allEntries() const {
    std::vector<std::pair<std::string, DedupEntry>> result;
    result.reserve(rows_.size());
    for (const auto& row : rows_) {
        result.emplace_back(row.commandId, row.entry);
    }
    return result;
}

PersistentDedupCache::PersistentDedupCache(IPersistentStore& store, size_t capacity)
    : store_(store), memory_(capacity) {
    load();
}

std::optional<DedupEntry> PersistentDedupCache::find(const std::string& commandId) {
    return memory_.find(commandId);
}

void PersistentDedupCache::record(const std::string& commandId, const DedupEntry& entry) {
    memory_.record(commandId, entry);
    save();
}

void PersistentDedupCache::load() {
    auto raw = store_.get(kStorageKey);
    if (!raw.has_value() || raw->empty()) {
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, *raw)) {
        return; // corrupt/absent - start with an empty cache
    }

    JsonArrayConst array = doc.as<JsonArrayConst>();
    for (JsonObjectConst row : array) {
        if (!row["id"].is<const char*>() || !row["status"].is<int>()) {
            continue;
        }
        DedupEntry entry;
        entry.status = static_cast<CommandStatus>(row["status"].as<int>());
        entry.hasError = row["has_error"].is<bool>() && row["has_error"].as<bool>();
        if (entry.hasError && row["error_code"].is<int>()) {
            entry.errorCode = static_cast<ProtocolErrorCode>(row["error_code"].as<int>());
        }
        memory_.record(row["id"].as<std::string>(), entry);
    }
}

void PersistentDedupCache::save() const {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (const auto& [commandId, entry] : memory_.allEntries()) {
        JsonObject obj = array.add<JsonObject>();
        obj["id"] = commandId;
        obj["status"] = static_cast<int>(entry.status);
        obj["has_error"] = entry.hasError;
        if (entry.hasError) {
            obj["error_code"] = static_cast<int>(entry.errorCode);
        }
    }

    std::string out;
    serializeJson(doc, out);
    store_.set(kStorageKey, out);
}

} // namespace interbridge
