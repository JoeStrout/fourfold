// Hashing and comparisons for secrets (over Monocypher's BLAKE2b): session
// IDs are stored hashed, CSRF tokens are keyed hashes, and both are compared
// in constant time. Random bytes come from platform/random.h.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fourfold {

constexpr size_t kHashSize = 32;

// BLAKE2b-256 of `data`: 32 raw bytes.
std::string hashBytes(std::string_view data);

// BLAKE2b-256 of `message`, keyed with `key` (1 to 64 bytes): a MAC, so
// only someone holding the key can make one that matches.
std::string keyedHash(std::string_view key, std::string_view message);

// Whether `a` and `b` are equal, taking the same time wherever they differ.
// (Their lengths aren't secret: unequal lengths return at once.)
bool constantTimeEqual(std::string_view a, std::string_view b);

// Lowercase hex.
std::string toHex(std::string_view bytes);

// Base64url without padding (RFC 4648 §5): the form tokens take in cookies,
// form fields and headers.
std::string toBase64Url(std::string_view bytes);
// The bytes, or nullopt if `text` isn't canonical unpadded base64url.
std::optional<std::string> fromBase64Url(std::string_view text);

}  // namespace fourfold
