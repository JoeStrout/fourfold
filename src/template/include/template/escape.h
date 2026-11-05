// Escapers, and the URL rule (overview §4, "Where fields are allowed"). Used
// by the template renderer and the Markdown renderer, so a value is treated
// the same way wherever it ends up.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fourfold {

// &, <, >, ", and ' as character references. Safe in text, in RCDATA
// (<title>, <textarea>), and in quoted attribute values.
std::string escapeHtml(std::string_view text);

// Percent-encodes every byte except A–Z a–z 0–9 - _ . ~, as for one URL
// component (a path segment, a query value). ':' and '/' are encoded too, so
// the result can't start a scheme or a new path.
std::string percentEncode(std::string_view text);

// The URL rule, for a URL whose start comes from a value: after dropping
// leading spaces and control characters, it must be relative or use http,
// https or mailto, and contain no other control characters (browsers drop
// tabs and newlines, so "java\tscript:" would otherwise get through).
// Returns the URL normalized (spaces, quotes, <, >, backslashes, non-ASCII
// and the like percent-encoded; existing %XX escapes kept), or nullopt.
std::optional<std::string> filterUrl(std::string_view url);

// A piece of a URL after its start (no scheme check): normalized as
// filterUrl normalizes a whole URL.
std::string normalizeUrlPiece(std::string_view piece);

// What a URL that fails the rule becomes: inert, and easy to search for.
extern const char* const kInvalidUrl;   // "about:invalid#fourfold"

// The URL's scheme, lowercased ("https"), or "" if it's relative. A URL is
// relative if no ':' comes before the first '/', '?' or '#'.
std::string urlScheme(std::string_view url);

// --- Markdown output (overview §4, Fields in Markdown) ---

// A value as Markdown text, which a Markdown parser reads back as exactly
// that text: every ASCII punctuation character backslash-escaped; CR, LF and
// tab as spaces (CRLF as one); leading and trailing spaces as &#32;, so they
// can't start a code block or make a hard line break. `followedByDot`: the
// text after it starts with '.', so a final "www" is written with an entity
// (it would make a web address).
std::string escapeMarkdownText(std::string_view value, bool followedByDot = false);

// A URL that passed filterUrl, made safe as a Markdown link destination:
// '(' and ')' percent-encoded too (filterUrl already encodes spaces, <, >,
// backslashes and control characters).
std::string markdownDestination(std::string_view url);

}  // namespace fourfold
