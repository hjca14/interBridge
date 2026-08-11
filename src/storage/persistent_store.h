#pragma once

#include <optional>
#include <string>

namespace interbridge {

// Generic key-value persistent storage abstraction. Used for anything
// that must survive reboot: Wi-Fi credentials, device identity, the AWS
// IoT certificate/private key, provisioning state, local configuration,
// command dedup state, and the event outbox.
//
// ESP32 NVS (via the Arduino Preferences API) is the expected real
// backend - see NvsStore in nvs_store.h, currently a stub.
class IPersistentStore {
public:
    virtual ~IPersistentStore() = default;

    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& key) = 0;
    virtual bool has(const std::string& key) const = 0;
};

} // namespace interbridge
