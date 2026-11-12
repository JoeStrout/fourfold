#include "outbound/address_rules.h"

#include <arpa/inet.h>

#include <cstring>

namespace fourfold {

namespace {

using Bytes = std::array<unsigned char, 16>;

bool startsWith(const Bytes& a, std::initializer_list<unsigned char> prefix, int bits) {
    int i = 0;
    for (unsigned char p : prefix) {
        int take = bits - i * 8;
        if (take <= 0) break;
        unsigned char mask = take >= 8 ? 0xFF : static_cast<unsigned char>(0xFF << (8 - take));
        if ((a[i] & mask) != (p & mask)) return false;
        i++;
    }
    return true;
}

std::string blockedV4(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
    if (a == 0) return "a \"this network\" address (0.0.0.0/8)";
    if (a == 10) return "a private address (10.0.0.0/8)";
    if (a == 100 && (b & 0xC0) == 64) return "a carrier-grade NAT address (100.64.0.0/10)";
    if (a == 127) return "a loopback address (127.0.0.0/8)";
    if (a == 169 && b == 254) return "a link-local address (169.254.0.0/16, where cloud metadata services live)";
    if (a == 172 && (b & 0xF0) == 16) return "a private address (172.16.0.0/12)";
    if (a == 192 && b == 0 && c == 0) return "an IETF protocol address (192.0.0.0/24)";
    if (a == 192 && b == 0 && c == 2) return "a documentation address (192.0.2.0/24)";
    if (a == 192 && b == 88 && c == 99) return "a 6to4 relay address (192.88.99.0/24)";
    if (a == 192 && b == 168) return "a private address (192.168.0.0/16)";
    if (a == 198 && (b & 0xFE) == 18) return "a benchmarking address (198.18.0.0/15)";
    if (a == 198 && b == 51 && c == 100) return "a documentation address (198.51.100.0/24)";
    if (a == 203 && b == 0 && c == 113) return "a documentation address (203.0.113.0/24)";
    if ((a & 0xF0) == 224) return "a multicast address (224.0.0.0/4)";
    if (a == 255 && b == 255 && c == 255 && d == 255) return "the broadcast address";
    if ((a & 0xF0) == 240) return "a reserved address (240.0.0.0/4)";
    return "";
}

std::string inside(const std::string& why, const char* how) {
    return why.empty() ? why : why + ", inside " + how;
}

}  // namespace

std::string blockedAddressReason(const Bytes& x) {
    // IPv4, mapped (::ffff:a.b.c.d).
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(x.data(), kMapped, 12) == 0) return blockedV4(x[12], x[13], x[14], x[15]);

    bool zero12 = true;
    for (int i = 0; i < 12; i++) zero12 = zero12 && x[i] == 0;
    if (zero12) {
        if (x[12] == 0 && x[13] == 0 && x[14] == 0 && x[15] == 0) return "the unspecified address (::)";
        if (x[12] == 0 && x[13] == 0 && x[14] == 0 && x[15] == 1) return "the loopback address (::1)";
        // IPv4-compatible (::a.b.c.d), deprecated but still routed by some stacks.
        std::string why = blockedV4(x[12], x[13], x[14], x[15]);
        return why.empty() ? "an IPv4-compatible IPv6 address (::/96, deprecated)"
                           : inside(why, "an IPv4-compatible IPv6 address");
    }
    // NAT64's well-known prefix, 64:ff9b::/96: judged by the IPv4 address.
    static const unsigned char kNat64[12] = {0, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
    if (std::memcmp(x.data(), kNat64, 12) == 0) return inside(blockedV4(x[12], x[13], x[14], x[15]), "a NAT64 address");
    if (startsWith(x, {0, 0x64, 0xFF, 0x9B, 0, 1}, 48)) return "a local-use NAT64 address (64:ff9b:1::/48)";
    if (startsWith(x, {0x01, 0x00, 0, 0, 0, 0, 0, 0}, 64)) return "a discard-only address (100::/64)";
    // 6to4, 2002::/16: the IPv4 address is in bytes 2-5.
    if (startsWith(x, {0x20, 0x02}, 16)) return inside(blockedV4(x[2], x[3], x[4], x[5]), "a 6to4 address");
    if (startsWith(x, {0x20, 0x01, 0x0D, 0xB8}, 32)) return "a documentation address (2001:db8::/32)";
    if (startsWith(x, {0x3F, 0xFF}, 20)) return "a documentation address (3fff::/20)";
    if (startsWith(x, {0x20, 0x01, 0x00, 0x00}, 32)) return "a Teredo address (2001::/32)";
    if (startsWith(x, {0x20, 0x01, 0x00}, 23)) return "an IETF protocol address (2001::/23)";
    if (startsWith(x, {0xFC}, 7)) return "a unique-local address (fc00::/7)";
    if (startsWith(x, {0xFE, 0x80}, 10)) return "a link-local address (fe80::/10)";
    if (startsWith(x, {0xFE, 0xC0}, 10)) return "a site-local address (fec0::/10)";
    if (x[0] == 0xFF) return "a multicast address (ff00::/8)";
    return "";
}

std::string formatAddress(const Bytes& x) {
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    char text[INET6_ADDRSTRLEN] = {};
    if (std::memcmp(x.data(), kMapped, 12) == 0) {
        inet_ntop(AF_INET, x.data() + 12, text, sizeof text);
    } else {
        inet_ntop(AF_INET6, x.data(), text, sizeof text);
    }
    return text;
}

std::string blockedAddressReason(const Bytes& address, const std::vector<AddressRange>& allowed) {
    std::string why = blockedAddressReason(address);
    if (why.empty()) return why;
    std::string text = formatAddress(address);
    for (const AddressRange& r : allowed) {
        if (r.contains(text)) return "";
    }
    return why;
}

}  // namespace fourfold
