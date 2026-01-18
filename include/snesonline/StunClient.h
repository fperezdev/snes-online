#pragma once

#include <cstdint>
#include <string>

namespace snesonline {

struct StunMappedAddress {
    std::string ip;
    uint16_t port = 0;
};

// Best-effort STUN (RFC 5389) binding request to discover the public mapped UDP endpoint
// for a socket bound to `localBindPort`.
//
// Notes:
// - NATs may use different mappings per destination (symmetric NAT). In that case, the
//   discovered port may not match the port seen by the peer.
// - This is still useful for many consumer NATs and avoids requiring your own UDP WHOAMI.
bool stunDiscoverMappedAddress(const char* stunHost, uint16_t stunPort, uint16_t localBindPort, StunMappedAddress& out,
                              int timeoutMs = 1200);

// Tries a small built-in list of public STUN servers.
bool stunDiscoverMappedAddressDefault(uint16_t localBindPort, StunMappedAddress& out, int timeoutPerServerMs = 1200);

} // namespace snesonline
