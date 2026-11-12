// The always-on network protections (overview §6): addresses an outbound
// request may never reach, whatever the allowlist says.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "platform/address.h"

namespace fourfold {

// Why the engine won't connect to `address` (16 bytes: IPv6, or IPv4 mapped
// into it, as parseAddress gives), or "" if it may. Refused: unspecified
// and "this network" (0.0.0.0/8, ::), loopback, private (RFC 1918), CGNAT
// (100.64.0.0/10), link-local (169.254.0.0/16, where cloud metadata lives;
// fe80::/10), IETF and documentation ranges, benchmarking (198.18.0.0/15),
// multicast, reserved (240.0.0.0/4) and broadcast; IPv6 unique-local
// (fc00::/7), site-local, discard-only, Teredo and the local-use NAT64
// prefix. An IPv6 address that carries an IPv4 one (IPv4-mapped,
// IPv4-compatible, NAT64's 64:ff9b::/96, 6to4's 2002::/16) is judged by the
// IPv4 address inside.
std::string blockedAddressReason(const std::array<unsigned char, 16>& address);

// The same, unless one of `allowed` (the config's allowAddresses) contains
// the address.
std::string blockedAddressReason(const std::array<unsigned char, 16>& address,
                                 const std::vector<AddressRange>& allowed);

// An address as text ("10.0.0.1", "2001:db8::1").
std::string formatAddress(const std::array<unsigned char, 16>& address);

}  // namespace fourfold
