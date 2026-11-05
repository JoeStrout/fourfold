#pragma once

#include <string>
#include <string_view>

namespace fourfold {

// True if `text` is well-formed UTF-8: no stray continuation bytes, no
// truncated sequences, no overlong encodings, no surrogates (U+D800–DFFF),
// nothing above U+10FFFF. Overlong encodings matter for security: "%c0%ae"
// is an overlong ".", and must not slip past a check for dot segments.
bool isValidUtf8(std::string_view text);

// `text` with each byte that isn't part of well-formed UTF-8 replaced by
// U+FFFD.
std::string toValidUtf8(std::string_view text);

// `text` (well-formed UTF-8) in Unicode Normalization Form C, so that "é"
// typed as one character and as "e" plus a combining accent compare equal
// (over utf8proc). Ill-formed input is made well formed first.
std::string toNfc(std::string_view text);

// The number of characters (code points) in well-formed UTF-8.
size_t utf8Length(std::string_view text);

}  // namespace fourfold
