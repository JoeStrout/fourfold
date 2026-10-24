// CompiledApp: an immutable description of one compiled version of the app
// (Arch §6). It's the only thing the engine knows about the app's code.
// Plain data and lookups, no logic; the compiler builds it.
//
// M2 holds routes, static files, page templates, and the compiled scripts.
// Queries, access rules and the rest arrive with their milestones.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "partition/paths.h"
#include "template/template.h"

namespace fourfold {

// The compiled .ms files of one app generation. Defined by the script
// component, which owns the bytecode (it lives in MiniScript's GC, on the
// script thread); to everything else it's opaque (Arch §6, "The script half").
class ScriptBundle;

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
    Page,      // a page: an .html template, its .ms file's render, or both
    Static,    // a static file, served with its content type from StaticIndex
    Handler,   // get.x and/or post.x in a page's .ms file
};

const char* routeKindName(RouteKind kind);

// A function in a page's .ms file that a URL can reach (overview §4,
// Handler model): the global `render`, or a function in the `get` or `post`
// map.
struct HandlerRef {
    enum class Kind { Render, Get, Post };

    Kind kind = Kind::Render;
    RelPath source;      // "pages/guestbook.ms"
    std::string name;    // "render", or the key in get/post ("sign" for post.sign)

    // "render", "get.more", "post.sign"
    std::string label() const;
};

struct RouteEntry {
    RouteKind kind;
    // The file in code/ this route comes from: the .html for a page that has
    // one (otherwise its .ms), the static file, or the .ms for a handler.
    RelPath source;
    // The route's one canonical URL ends in '/': true for a folder's index
    // page (/docs/ for pages/docs/index.html), so relative links in it
    // resolve inside the folder. The other form redirects to this one.
    bool trailingSlash = false;
    std::optional<HandlerRef> render = std::nullopt;   // Page: its .ms file's render, if any
    std::optional<HandlerRef> get = std::nullopt;      // Handler: get.x, if any
    std::optional<HandlerRef> post = std::nullopt;     // Handler: post.x, if any
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

// The parsed .html templates in code/pages/, by source path.
class TemplateSet {
public:
    void add(RelPath source, CompiledTemplate compiled);
    const CompiledTemplate* find(const RelPath& source) const;
    size_t size() const { return templates_.size(); }

private:
    std::map<RelPath, CompiledTemplate> templates_;
};

struct CompiledApp {
    uint64_t generation = 0;
    RouteTable routes;
    StaticIndex statics;
    TemplateSet templates;
    std::optional<RelPath> notFoundPage;    // pages/404.html, if present
    std::optional<RelPath> errorPage;       // pages/500.html, if present
    // Null if the app has no .ms files. Shared by every request on this
    // generation; the bytecode is released when the last one lets go.
    std::shared_ptr<const ScriptBundle> scripts;
    std::vector<Diagnostic> diagnostics;    // warnings that didn't stop the build
};

// A readable listing of the app: every route, then the diagnostics. Used
// for golden tests, and later for the dev tools' route report.
std::string describe(const CompiledApp& app);

}  // namespace fourfold
