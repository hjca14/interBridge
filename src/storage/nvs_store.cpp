#include "nvs_store.h"

namespace interbridge {

std::optional<std::string> NvsStore::get(const std::string& key) const {
    (void)key;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
    return std::nullopt;
}

void NvsStore::set(const std::string& key, const std::string& value) {
    (void)key;
    (void)value;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
}

void NvsStore::remove(const std::string& key) {
    (void)key;
    // TODO: not implemented. See CONTEXT.md > Open Questions.
}

bool NvsStore::has(const std::string& key) const {
    (void)key;
    return false;
}

} // namespace interbridge
