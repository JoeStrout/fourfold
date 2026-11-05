// IP addresses and ranges, for trusting a reverse proxy's forwarded
// addresses (overview §8, Trusting the client's address).
#pragma once

#include <array>
#include <optional>
#include <string>

namespace fourfold {

// An address ("10.0.0.1", "::1") or a range in CIDR form ("10.0.0.0/8",
// "fd00::/8"). An IPv4-mapped IPv6 address (::ffff:1.2.3.4) counts as the
// IPv4 address.
class AddressRange {
public:
    static std::optional<AddressRange> parse(const std::string& text);
    bool contains(const std::string& address) const;
    const std::string& text() const { return text_; }

private:
    std::array<unsigned char, 16> bytes_{};   // IPv4 in the last four, as mapped
    int prefixBits_ = 128;                     // of the 128
    std::string text_;
};

// A numeric address as 16 bytes (IPv4 mapped into IPv6), or nullopt if it
// isn't one.
std::optional<std::array<unsigned char, 16>> parseAddress(const std::string& text);

}  // namespace fourfold
