#pragma once

#include "persistent_store.h"

namespace interbridge {

// Real ESP32 implementation. STUB: intended to wrap the Arduino
// Preferences API (ESP32 NVS) but not implemented yet - no namespace/key
// layout has been decided, and NVS has per-entry size limits that the
// callers of IPersistentStore (certificate/private key storage, the
// event outbox) have not been validated against. See CONTEXT.md > Open
// Questions.
class NvsStore : public IPersistentStore {
public:
    std::optional<std::string> get(const std::string& key) const override;
    void set(const std::string& key, const std::string& value) override;
    void remove(const std::string& key) override;
    bool has(const std::string& key) const override;
};

} // namespace interbridge
