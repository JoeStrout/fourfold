// The content types Fourfold serves static files as. It's an allowlist: a
// file whose extension isn't here isn't served at all (the compiler warns),
// so a browser never has to guess a type, and stray files (backups, notes,
// source files) don't become public by accident.
#pragma once

#include <string_view>

namespace fourfold {

// The Content-Type for an extension (with its dot, any case: ".PNG"), or
// nullptr if it isn't on the list.
const char* contentTypeForExtension(std::string_view extension);

}  // namespace fourfold
