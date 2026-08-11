#pragma once

#include "../storage/persistent_store.h"

namespace interbridge {

// Clears user provisioning/configuration data (Wi-Fi credentials, the
// provisioned flag) without touching the stable device_id or the AWS IoT
// certificate/private key - see docs/communication-protocol.md > Factory
// Reset ("preserves: stable device_id, permanent AWS IoT certificate,
// permanent AWS IoT private key"). Remote FACTORY_RESET is intentionally
// not implemented (see protocol/command_handler.*) - this coordinator is
// only reachable via the physical button's ~10s hold.
class FactoryResetCoordinator {
public:
    explicit FactoryResetCoordinator(IPersistentStore& store);

    // Clears Wi-Fi credentials and the provisioned flag. Returns true;
    // currently unconditional/synchronous, kept as bool for a future
    // implementation that might need to report failure (e.g. flush
    // errors).
    bool execute();

private:
    IPersistentStore& store_;
};

} // namespace interbridge
