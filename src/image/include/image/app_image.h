// AppImage: an immutable description of one compiled version of the app
// (Arch §6). It's the only thing the engine knows about the app's code.
// Plain data and lookups, no logic; the compiler builds it.
//
// M1 holds routes and static files only. Templates, scripts, queries and the
// rest arrive with their milestones.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "partition/paths.h"

namespace fourfold {

struct Diagnostic {
    enum class Severity { Error, Warning };

    Severity severity;
    std::string file;   // "code/pages/about.html", or "code" for the whole tree
    int line = 0;       // 0 if it isn't about a particular line
    std::string message;

    // "code/pages/about.html:3: error: message"
    std::string toString() const;
};

enum class RouteKind {
    Page,     // an .html page (served as is, in M1)
    Static,   // a static file, served with its content type from StaticIndex
};

const char* routeKindName(RouteKind kind);

struct RouteEntry {
    RouteKind kind;
    RelPath source;              // the file in code/ this route comes from
    // The route's one canonical URL ends in '/': true for a folder's index
    // page (/docs/ for pages/docs/index.html), so relative links in it
    // resolve inside the folder. The other form redirects to this one.
    bool trailingSlash = false;
};

// Canonical URL path (see canonicalUrlPath, which reports the trailing slash
// separately) → route. Lookups are exact: no patterns, no case folding
// (overview §5).
class RouteTable {
public:
    // False if `path` is already taken.
    bool add(std::string path, RouteEntry entry);
    const RouteEntry* find(const std::string& path) const;

    size_t size() const { return routes_.size(); }
    std::map<std::string, RouteEntry>::const_iterator begin() const { return routes_.begin(); }
    std::map<std::string, RouteEntry>::const_iterator end() const { return routes_.end(); }

private:
    std::map<std::string, RouteEntry> routes_;
};

struct StaticFile {
    std::string contentType;   // "image/png", "text/css; charset=utf-8"
    uint64_t size = 0;
    int64_t modifiedNs = 0;
};

// The static files in code/pages/, by source path.
class StaticIndex {
public:
    void add(RelPath source, StaticFile file);
    const StaticFile* find(const RelPath& source) const;
    size_t size() const { return files_.size(); }

private:
    std::map<RelPath, StaticFile> files_;
};

struct AppImage {
    uint64_t generation = 0;
    RouteTable routes;
    StaticIndex statics;
    std::optional<RelPath> notFoundPage;    // pages/404.html, if present
    std::vector<Diagnostic> diagnostics;    // warnings that didn't stop the build
};

// A readable listing of the image: every route, then the diagnostics. Used
// for golden tests, and later for the dev tools' route report.
std::string describe(const AppImage& image);

}  // namespace fourfold
