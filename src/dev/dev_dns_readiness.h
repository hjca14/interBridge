#pragma once

namespace interbridge {

// Whether attempting to resolve a hostname via WiFi.hostByName() is worth
// trying right now. The only real precondition is an associated STA
// interface with a valid local IP - see docs/dev-ble-mqtt.md's "DNS
// precondition" note for the physical bench failure this guards against.
// `WiFi.dnsIP()` is a separate Arduino-ESP32 wrapper value that is not
// reliably populated even once the STA interface is fully associated with
// a valid local IP; treating it as a hard precondition for even
// *attempting* hostByName() caused every attempt to be skipped on a real
// bench run against an endpoint independently confirmed resolvable
// (`nslookup`) - never gate on it here again.
bool isReadyToAttemptDnsResolution(bool wifiConnected, bool hasValidLocalIp);

} // namespace interbridge
