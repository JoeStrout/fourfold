// Paths: the one place Fourfold decides what a path may look like (Arch §5).
//
// RelPath is a path within a partition, like "pages/about.html", checked
// once when it's made. The file operations in this component take RelPath,
// never strings, so no other code needs to think about "..", "//", or
// encodings.
//
// canonicalUrlPath turns a request target into the form the route table is
// keyed by (overview §5, "Access is resolved per route").
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/result.h"

namespace fourfold {

class RelPath {
public:
    // Rules for each '/'-separated segment: not empty, not "." or "..", not
    // starting with "." (so dot-files, and data/.fourfold/, are unreachable),
    // no control characters or '\', valid UTF-8, at most 255 bytes. The
    // whole path is relative (no leading '/') and at most 1024 bytes.
    static constexpr size_t kMaxSegmentBytes = 255;
    static constexpr size_t kMaxPathBytes = 1024;

    // The empty path: the partition's root folder.
    RelPath() = default;

    // Error code Invalid, with the reason, if `path` breaks a rule.
    static Result<RelPath> parse(std::string_view path);

    const std::string& str() const { return path_; }
    bool isRoot() const { return path_.empty(); }
    std::vector<std::string_view> segments() const;

    // The last segment ("about.html"); empty for the root.
    std::string_view name() const;
    // The name's extension, with its dot (".html"); empty if none.
    std::string_view extension() const;
    // The containing folder; the root's parent is the root.
    RelPath parent() const;
    // This path plus one more segment.
    Result<RelPath> child(std::string_view name) const;
    // True if this is `folder` or lies somewhere below it.
    bool isWithin(const RelPath& folder) const;

    bool operator==(const RelPath& other) const { return path_ == other.path_; }
    bool operator!=(const RelPath& other) const { return path_ != other.path_; }
    bool operator<(const RelPath& other) const { return path_ < other.path_; }

private:
    explicit RelPath(std::string path) : path_(std::move(path)) {}
    std::string path_;
};

struct UrlPath {
    std::string path;            // "/", or "/a/b" with no trailing slash
    bool trailingSlash = false;  // the request had one ("/a/b/")
};

// Canonicalizes the path part of a request target ("/a/b?x=1"): drops the
// query and fragment, percent-decodes each segment, and collapses repeated
// slashes. Rejects (error code Invalid) anything not starting with '/', bad
// percent escapes, an encoded '/' ("%2F"), "." and ".." segments (however
// encoded), control characters, '\', and invalid UTF-8. Never changes case.
Result<UrlPath> canonicalUrlPath(std::string_view target);

// The reverse, for building URLs (e.g. a redirect's Location) from a
// canonical path: percent-encodes every byte that isn't safe in a URL path,
// leaving '/' as the separator. Use this, never the raw request target: a
// raw "//evil.com" in a Location header would send the browser off-site.
std::string encodeUrlPath(std::string_view path);

}  // namespace fourfold
