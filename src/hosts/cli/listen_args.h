// Command-line values shared by the commands that listen.
#pragma once

#include <cstdint>
#include <string>

namespace fourfold::cli {

// A port number, 1 to 65535.
inline bool parsePort(const std::string& p, uint16_t& out) {
    bool digits = !p.empty() && p.size() <= 5 && p.find_first_not_of("0123456789") == std::string::npos;
    unsigned long n = digits ? std::stoul(p) : 0;
    if (n == 0 || n > 65535) return false;
    out = static_cast<uint16_t>(n);
    return true;
}

// ADDRESS:PORT, with an IPv6 address in brackets: [::]:8090.
inline bool parseListen(const std::string& v, std::string& address, uint16_t& port) {
    size_t colon = v.rfind(':');
    if (colon == std::string::npos || !parsePort(v.substr(colon + 1), port)) return false;
    address = v.substr(0, colon);
    if (address.size() >= 2 && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    return !address.empty();
}

}  // namespace fourfold::cli
