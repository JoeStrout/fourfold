#include "base/utf8.h"

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

}  // namespace fourfold
