#include "base/utf8.h"

#include <cstdlib>

#include "utf8proc.h"

namespace fourfold {

bool isValidUtf8(std::string_view text) {
    size_t i = 0, n = text.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t length;
        unsigned char lo = 0x80, hi = 0xBF;   // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            if (c == 0xE0) lo = 0xA0;          // overlong
            if (c == 0xED) hi = 0x9F;          // surrogates
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            if (c == 0xF0) lo = 0x90;          // overlong
            if (c == 0xF4) hi = 0x8F;          // above U+10FFFF
        } else {
            return false;                      // continuation byte, C0/C1, or F5+
        }
        if (i + length > n) return false;
        unsigned char second = static_cast<unsigned char>(text[i + 1]);
        if (second < lo || second > hi) return false;
        for (size_t k = 2; k < length; k++) {
            unsigned char b = static_cast<unsigned char>(text[i + k]);
            if (b < 0x80 || b > 0xBF) return false;
        }
        i += length;
    }
    return true;
}

std::string toValidUtf8(std::string_view s) {
    if (isValidUtf8(s)) return std::string(s);
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        // The longest valid sequence starting here (at most 4 bytes).
        size_t len = 0;
        for (size_t n = 1; n <= 4 && i + n <= s.size(); n++) {
            if (isValidUtf8(s.substr(i, n))) {
                len = n;
                break;
            }
        }
        if (len) {
            out.append(s.substr(i, len));
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
        }
    }
    return out;
}

std::string toNfc(std::string_view text) {
    bool ascii = true;
    for (unsigned char c : text) ascii = ascii && c < 0x80;
    if (ascii) return std::string(text);   // already NFC, and the common case
    std::string valid = toValidUtf8(text);
    utf8proc_uint8_t* out = nullptr;
    utf8proc_ssize_t n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(valid.data()),
                                      static_cast<utf8proc_ssize_t>(valid.size()), &out,
                                      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    if (n < 0 || !out) {
        std::free(out);
        return valid;   // only on running out of memory; the text is still well formed
    }
    std::string result(reinterpret_cast<const char*>(out), static_cast<size_t>(n));
    std::free(out);
    return result;
}

size_t utf8Length(std::string_view text) {
    size_t n = 0;
    for (unsigned char c : text) n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace fourfold
