// The content types Fourfold serves static files as. It's an allowlist: a
// file whose extension isn't here isn't served at all (the compiler warns),
// so a browser never has to guess a type, and stray files (backups, notes,
// source files) don't become public by accident.
#pragma once

#include <string>
#include <string_view>

namespace fourfold {

// The Content-Type for an extension (with its dot, any case: ".PNG"), or
// nullptr if it isn't on the list.
const char* contentTypeForExtension(std::string_view extension);

// Checks a type an app adds for an extension in code/static.ms (M11.2):
// `extension` ("unityweb" or ".unityweb", any case) and `contentType`
// ("application/octet-stream"). Returns the extension as looked up (".unityweb")
// and sets `contentType` to its normal form, or returns "" with `error` set:
// an extension Fourfold knows (built-in types can't be changed), one that's
// a page or code (.html, .md, .ms), or a type that would run script as the
// site (HTML, XHTML, SVG, JavaScript). `sandbox` is set for XML types
// (*/xml, */*+xml), which are served with a CSP sandbox.
std::string checkAddedType(std::string_view extension, std::string& contentType, bool& sandbox, std::string& error);

}  // namespace fourfold
