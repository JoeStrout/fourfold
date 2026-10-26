#include "platform/random.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h>   // arc4random_buf
#define FOURFOLD_HAVE_ARC4RANDOM 1
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>   // getrandom
#endif

namespace fourfold {

void secureRandomBytes(void* out, size_t size) {
#if defined(FOURFOLD_HAVE_ARC4RANDOM)
    arc4random_buf(out, size);   // can't fail
#elif defined(__linux__)
    auto* p = static_cast<unsigned char*>(out);
    while (size > 0) {
        ssize_t n = getrandom(p, size, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "fourfold: getrandom failed; can't continue without secure random numbers\n");
            std::abort();
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
#else
#error "secureRandomBytes isn't implemented for this OS"
#endif
}

std::string randomBase64(size_t bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> raw(bytes);
    secureRandomBytes(raw.data(), raw.size());
    std::string out;
    for (size_t i = 0; i < raw.size(); i += 3) {
        unsigned n = unsigned(raw[i]) << 16;
        if (i + 1 < raw.size()) n |= unsigned(raw[i + 1]) << 8;
        if (i + 2 < raw.size()) n |= raw[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < raw.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

}  // namespace fourfold
