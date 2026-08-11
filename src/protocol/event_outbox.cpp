#include "event_outbox.h"

#include <algorithm>

#define ARDUINOJSON_ENABLE_STD_STRING 1
#include <ArduinoJson.h>

namespace {
constexpr const char* kStorageKey = "event_outbox";
}

namespace interbridge {

MemoryEventOutbox::MemoryEventOutbox(size_t capacity) : capacity_(capacity) {}

void MemoryEventOutbox::enqueue(const std::string& eventId, const std::string& eventJson) {
    for (auto& entry : entries_) {
        if (entry.eventId == eventId) {
            entry.eventJson = eventJson;
            return;
        }
    }
    if (entries_.size() >= capacity_) {
        entries_.pop_front();
    }
    entries_.push_back(OutboxEntry{eventId, eventJson});
}

std::vector<OutboxEntry> MemoryEventOutbox::pending() const {
    return std::vector<OutboxEntry>(entries_.begin(), entries_.end());
}

void MemoryEventOutbox::dequeue(const std::string& eventId) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                   [&](const OutboxEntry& e) { return e.eventId == eventId; }),
                    entries_.end());
}

size_t MemoryEventOutbox::size() const {
    return entries_.size();
}

PersistentEventOutbox::PersistentEventOutbox(IPersistentStore& store, size_t capacity)
    : store_(store), memory_(capacity) {
    load();
}

void PersistentEventOutbox::enqueue(const std::string& eventId, const std::string& eventJson) {
    memory_.enqueue(eventId, eventJson);
    save();
}

std::vector<OutboxEntry> PersistentEventOutbox::pending() const {
    return memory_.pending();
}

void PersistentEventOutbox::dequeue(const std::string& eventId) {
    memory_.dequeue(eventId);
    save();
}

size_t PersistentEventOutbox::size() const {
    return memory_.size();
}

void PersistentEventOutbox::load() {
    auto raw = store_.get(kStorageKey);
    if (!raw.has_value() || raw->empty()) {
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, *raw)) {
        return; // corrupt/absent - start with an empty outbox
    }

    JsonArrayConst array = doc.as<JsonArrayConst>();
    for (JsonObjectConst row : array) {
        if (!row["id"].is<const char*>() || !row["json"].is<const char*>()) {
            continue;
        }
        memory_.enqueue(row["id"].as<std::string>(), row["json"].as<std::string>());
    }
}

void PersistentEventOutbox::save() const {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (const auto& entry : memory_.pending()) {
        JsonObject obj = array.add<JsonObject>();
        obj["id"] = entry.eventId;
        obj["json"] = entry.eventJson;
    }

    std::string out;
    serializeJson(doc, out);
    store_.set(kStorageKey, out);
}

} // namespace interbridge
