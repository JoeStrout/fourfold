#include "platform/address.h"

#include <arpa/inet.h>
#include <cstring>

namespace fourfold {

std::optional<std::array<unsigned char, 16>> parseAddress(const std::string& text) {
    std::array<unsigned char, 16> out{};
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        out[10] = out[11] = 0xff;
        std::memcpy(out.data() + 12, &v4, 4);
        return out;
    }
    std::string bare = text;
    if (bare.size() > 2 && bare.front() == '[' && bare.back() == ']') bare = bare.substr(1, bare.size() - 2);
    in6_addr v6;
    if (inet_pton(AF_INET6, bare.c_str(), &v6) == 1) {
        std::memcpy(out.data(), v6.s6_addr, 16);
        return out;
    }
    return std::nullopt;
}

std::optional<AddressRange> AddressRange::parse(const std::string& text) {
    AddressRange r;
    r.text_ = text;
    size_t slash = text.find('/');
    auto address = parseAddress(text.substr(0, slash));
    if (!address) return std::nullopt;
    r.bytes_ = *address;
    bool v4 = text.find(':') == std::string::npos;
    if (slash != std::string::npos) {
        std::string bits = text.substr(slash + 1);
        if (bits.empty() || bits.size() > 3) return std::nullopt;
        int n = 0;
        for (char c : bits) {
            if (c < '0' || c > '9') return std::nullopt;
            n = n * 10 + (c - '0');
        }
        if (n > (v4 ? 32 : 128)) return std::nullopt;
        r.prefixBits_ = v4 ? 96 + n : n;
    }
    return r;
}

bool AddressRange::contains(const std::string& address) const {
    auto a = parseAddress(address);
    if (!a) return false;
    int bits = prefixBits_;
    for (int i = 0; i < 16 && bits > 0; i++, bits -= 8) {
        unsigned char mask = bits >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - bits));
        if (((*a)[i] & mask) != (bytes_[i] & mask)) return false;
    }
    return true;
}

}  // namespace fourfold
