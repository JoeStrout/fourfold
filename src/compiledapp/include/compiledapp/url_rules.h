// URL rules (M11.2): redirects and rewrites for URLs no route serves, from
// code/routes.ms, so a site moving in keeps its old URLs.
//
//   redirects["/wiki/{rest*}"] = "https://wiki.miniscript.org/wiki/{rest}"
//   rewrites["/tryit/{code}"]  = "/tryit/?s={code}"
//
// A pattern is a URL path whose segments are each either literal text or a
// placeholder: {name} matches one whole segment, {name*} the rest of the
// path (zero or more segments, a trailing slash included) and may only come
// last. No regular expressions. Literal text is written decoded ("/my page"),
// as the route table keys are, and matches exactly (no case folding).
//
// A target is a URL with placeholders from its pattern. Values are decoded
// when matched and encoded again when filled in, for the part of the URL
// they land in (path or query), so a value can't add a '/', '?' or '#' that
// wasn't in it; and a target can't begin with a placeholder, or put one
// before its path, so a value can never choose the host.
//
// The pattern matcher is also what M12's parameterized routes will use.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "partition/paths.h"

namespace fourfold {

// Placeholder values from a match, by name, in the pattern's order.
using UrlValues = std::vector<std::pair<std::string, std::string>>;

class UrlPattern {
public:
    struct Segment {
        std::string text;          // literal text, or the placeholder's name
        bool placeholder = false;  // {name}
        bool rest = false;         // {name*}: the rest of the path (last only)
    };

    // nullopt, with `error` saying why, if `text` isn't a pattern.
    static std::optional<UrlPattern> parse(const std::string& text, std::string& error);

    const std::string& text() const { return text_; }
    const std::vector<Segment>& segments() const { return segments_; }
    bool trailingSlash() const { return trailingSlash_; }
    bool hasPlaceholders() const;
    // The placeholders' names, in order.
    std::vector<std::string> names() const;
    // The literal segments before the first placeholder, as a path: "/tryit"
    // for "/tryit/{code}", "/" for "/{x}". The whole path if there are no
    // placeholders.
    std::string literalPrefix() const;

    // Whether the canonical path matches; fills `values` if so.
    bool match(const UrlPath& path, UrlValues& values) const;
    // Whether every path this pattern matches, `other` does too (so a rule
    // for this pattern after one for `other` is never reached).
    bool coveredBy(const UrlPattern& other) const;

private:
    std::string text_;
    std::vector<Segment> segments_;
    bool trailingSlash_ = false;
};

class UrlTarget {
public:
    enum class Kind {
        Local,      // "/path?query"
        Absolute,   // "https://host/path?query"
    };

    // nullopt, with `error`, if `text` isn't a target, or names a
    // placeholder `pattern` doesn't have.
    static std::optional<UrlTarget> parse(const std::string& text, const UrlPattern& pattern, std::string& error);

    const std::string& text() const { return text_; }
    Kind kind() const { return kind_; }
    bool hasPathPlaceholders() const;
    // The path, for a local target with no placeholders in it: decoded and
    // canonical, as a route is keyed ("/tryit"), with its trailing slash in
    // `trailingSlash`. nullopt otherwise.
    std::optional<UrlPath> fixedPath() const;

    // The target with `values` filled in: path placeholders encoded as path
    // (a rest value keeps its '/'), query ones as a query component. Repeated
    // slashes in the path are collapsed, so an empty value can't make "//".
    std::string fill(const UrlValues& values) const;

private:
    struct Part {
        std::string text;          // literal text, or a placeholder's name
        bool placeholder = false;
        bool inQuery = false;      // after the '?' (or '#')
    };

    std::string text_;
    Kind kind_ = Kind::Local;
    size_t pathStart_ = 0;   // where the path begins in a filled Absolute target
    std::vector<Part> parts_;
};

struct UrlRule {
    enum class Kind { Redirect, Rewrite };

    Kind kind = Kind::Redirect;
    UrlPattern pattern;
    UrlTarget target;
    int status = 301;     // Redirect: 301, 302, 307 or 308
    // Rewrite: the route served (its canonical path, as the route table is
    // keyed, and whether it's a Markdown page's Markdown URL).
    std::string routePath;
    bool markdownAlias = false;
    // "code/routes.ms: rewrites[\"/tryit/{code}\"]"
    std::string source;
};

// Encodes a value for a URL's query string: everything but RFC 3986's
// unreserved characters.
std::string encodeQueryComponent(std::string_view value);

}  // namespace fourfold
