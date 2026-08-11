#pragma once

#include <map>

#include "persistent_store.h"

namespace interbridge {

// In-memory IPersistentStore for native tests and local development.
// Nothing survives process exit.
class MemoryStore : public IPersistentStore {
public:
    std::optional<std::string> get(const std::string& key) const override;
    void set(const std::string& key, const std::string& value) override;
    void remove(const std::string& key) override;
    bool has(const std::string& key) const override;

private:
    std::map<std::string, std::string> values_;
};

} // namespace interbridge
