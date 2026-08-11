#include "memory_store.h"

namespace interbridge {

std::optional<std::string> MemoryStore::get(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MemoryStore::set(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void MemoryStore::remove(const std::string& key) {
    values_.erase(key);
}

bool MemoryStore::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

} // namespace interbridge
