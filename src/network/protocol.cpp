#include "protocol.h"

namespace interbridge {

bool NullProtocol::isConnected() const {
    return false;
}

bool NullProtocol::send(const ProtocolMessage& message) {
    (void)message;
    return false;
}

void NullProtocol::update() {}

} // namespace interbridge
