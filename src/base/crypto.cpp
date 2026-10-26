#include "base/crypto.h"

#include <algorithm>
#include <cstdint>

#include "monocypher.h"

namespace fourfold {

namespace {

const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const uint8_t* bytesOf(std::string_view s) { return reinterpret_cast<const uint8_t*>(s.data()); }

int base64UrlValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

}  // namespace

std::string hashBytes(std::string_view data) {
    std::string out(kHashSize, '\0');
    crypto_blake2b(reinterpret_cast<uint8_t*>(out.data()), kHashSize, bytesOf(data), data.size());
    return out;
}

std::string keyedHash(std::string_view key, std::string_view message) {
    std::string out(kHashSize, '\0');
    crypto_blake2b_keyed(reinterpret_cast<uint8_t*>(out.data()), kHashSize, bytesOf(key), key.size(),
                         bytesOf(message), message.size());
    return out;
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string toHex(std::string_view bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += kHex[c >> 4];
        out += kHex[c & 0xF];
    }
    return out;
}

std::string toBase64Url(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size()) n |= uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 2]);
        out += kBase64Url[(n >> 18) & 63];
        out += kBase64Url[(n >> 12) & 63];
        if (i + 1 < bytes.size()) out += kBase64Url[(n >> 6) & 63];
        if (i + 2 < bytes.size()) out += kBase64Url[n & 63];
    }
    return out;
}

std::optional<std::string> fromBase64Url(std::string_view text) {
    if (text.size() % 4 == 1) return std::nullopt;
    std::string out;
    out.reserve(text.size() * 3 / 4);
    for (size_t i = 0; i < text.size(); i += 4) {
        size_t n = std::min<size_t>(4, text.size() - i);
        uint32_t bits = 0;
        for (size_t j = 0; j < 4; j++) {
            int v = 0;
            if (j < n) {
                v = base64UrlValue(text[i + j]);
                if (v < 0) return std::nullopt;
            }
            bits = (bits << 6) | static_cast<uint32_t>(v);
        }
        out += static_cast<char>((bits >> 16) & 0xFF);
        if (n > 2) out += static_cast<char>((bits >> 8) & 0xFF);
        if (n > 3) out += static_cast<char>(bits & 0xFF);
        // Canonical: the unused low bits of a short final group are zero.
        if (n == 2 && (bits & 0xFFFF) != 0) return std::nullopt;
        if (n == 3 && (bits & 0xFF) != 0) return std::nullopt;
    }
    return out;
}

}  // namespace fourfold
