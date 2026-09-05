#include "dev_dns_readiness.h"

namespace interbridge {

bool isReadyToAttemptDnsResolution(bool wifiConnected, bool hasValidLocalIp) {
    return wifiConnected && hasValidLocalIp;
}

} // namespace interbridge
