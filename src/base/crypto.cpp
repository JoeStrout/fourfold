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

namespace {

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

}  // namespace

std::string sha256(std::string_view data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = data.size() / 64 * 64;
    for (size_t i = 0; i < full; i += 64) sha256Block(h, bytesOf(data) + i);
    // The rest, the 0x80 byte, zeros, and the length in bits.
    uint8_t tail[128] = {};
    size_t rest = data.size() - full;
    for (size_t i = 0; i < rest; i++) tail[i] = static_cast<uint8_t>(data[full + i]);
    tail[rest] = 0x80;
    size_t tailSize = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; i++) tail[tailSize - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    for (size_t i = 0; i < tailSize; i += 64) sha256Block(h, tail + i);
    std::string out(32, '\0');
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<char>(h[i] >> (24 - 8 * j));
    }
    return out;
}

int leadingZeroBits(std::string_view bytes) {
    int n = 0;
    for (unsigned char c : bytes) {
        if (c == 0) {
            n += 8;
            continue;
        }
        while (!(c & 0x80)) {
            n++;
            c = static_cast<unsigned char>(c << 1);
        }
        break;
    }
    return n;
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
