#pragma once

#include <cstddef>
#include <cstdint>

namespace interbridge {

// Raw message exchanged with the InterBridge backend/app, independent of
// Wi-Fi (see network/wifi.h). The wire format and transport protocol
// (MQTT, HTTP, WebSocket, a custom TCP protocol, ...) have not been
// decided yet. See CONTEXT.md > Open Questions.
struct ProtocolMessage {
    const uint8_t* data;
    size_t length;
};

class ICommunicationProtocol {
public:
    virtual ~ICommunicationProtocol() = default;

    virtual bool isConnected() const = 0;
    virtual bool send(const ProtocolMessage& message) = 0;
    virtual void update() = 0;
};

// Placeholder used until a concrete protocol is chosen. All operations
// are no-ops and must not be treated as functional; it exists so callers
// have a safe default to hold instead of a null/optional pointer.
class NullProtocol : public ICommunicationProtocol {
public:
    bool isConnected() const override;
    bool send(const ProtocolMessage& message) override;
    void update() override;
};

} // namespace interbridge
