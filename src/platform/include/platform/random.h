// Random bytes from the operating system's cryptographic generator, for
// nonces and (later) session IDs and tokens.
#pragma once

#include <cstddef>
#include <string>

namespace fourfold {

// Fills `out` with `size` unpredictable bytes. Aborts the process if the OS
// can't supply them: there's no safe fallback.
void secureRandomBytes(void* out, size_t size);

// `bytes` random bytes, as base64 (RFC 4648, with padding): the form a CSP
// nonce takes.
std::string randomBase64(size_t bytes);

}  // namespace fourfold
