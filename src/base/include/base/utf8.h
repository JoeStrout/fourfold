#pragma once

#include <string_view>

namespace fourfold {

// True if `text` is well-formed UTF-8: no stray continuation bytes, no
// truncated sequences, no overlong encodings, no surrogates (U+D800–DFFF),
// nothing above U+10FFFF. Overlong encodings matter for security: "%c0%ae"
// is an overlong ".", and must not slip past a check for dot segments.
bool isValidUtf8(std::string_view text);

}  // namespace fourfold
