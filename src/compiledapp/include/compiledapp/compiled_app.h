// CompiledApp: an immutable description of one compiled version of the app
// (Arch §6). It's the only thing the engine knows about the app's code.
// Plain data and lookups, no logic; the compiler builds it.
//
// It holds routes, static files, templates (pages and fragments), Markdown
// pages, and the compiled scripts. Queries, access rules and the rest arrive
// with their milestones.
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
    Markdown,  // a .md page, rendered at compile time and served in its layout
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

// Who may use a route (overview §5, Access control), already combined from
// every rule that applies: each requirement in `all` must be met (AND), and
// a requirement is met by any of its roles (OR). A requirement with no
// roles is "login": any signed-in user. No requirements: public.
struct AccessLevel {
    std::vector<std::vector<std::string>> all;   // each sorted, no repeats

    bool isPublic() const { return all.empty(); }
    // Adds a requirement (empty: "login"). Only ever tightens: a requirement
    // already implied by another (a superset of roles, or "login" once a
    // role is required) is dropped.
    void require(std::vector<std::string> anyOf);
    // Whether a visitor with these roles may use the route; `signedIn` false
    // means not signed in (no roles).
    bool allows(bool signedIn, const std::vector<std::string>& roles) const;
    // "public", "login", "admin", "admin or moderator", "login and editor"...
    std::string describe() const;

    bool operator==(const AccessLevel& o) const { return all == o.all; }
};

// A role name: a letter, then letters, digits, '_' or '-', at most 64
// bytes; and not "login" or "public", which mean something else in access
// rules.
bool isValidRoleName(const std::string& role);

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
    // The route's code uses the session (its .ms file, or a lib it imports,
    // names `session` or `flash`), or its page has a {{flash}} field: the
    // session is loaded, and locked while its code runs.
    bool usesSession = false;
    // Who may use it (overview §5), from code/access.ms and the page's
    // `access =`; `accessSources` names each rule that applied
    // ("code/access.ms: /admin", "code/pages/admin/index.ms").
    AccessLevel access{};
    std::vector<std::string> accessSources{};
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

// The parsed templates: pages in code/pages/ by source path, and fragments
// in code/frags/ by name ("entry" for frags/entry.html or entry.md), with
// their includes resolved.
class TemplateSet {
public:
    void add(RelPath source, CompiledTemplate compiled);
    const CompiledTemplate* find(const RelPath& source) const;
    size_t size() const { return templates_.size(); }

    void setFragments(FragmentMap fragments) { fragments_ = std::move(fragments); }
    const CompiledTemplate* fragment(const std::string& name) const;
    const FragmentMap& fragments() const { return fragments_; }

private:
    std::map<RelPath, CompiledTemplate> templates_;
    FragmentMap fragments_;
};

// A .md page in code/pages/, rendered once, at compile time.
struct MarkdownPage {
    Html content;
    std::string title;   // from its first # heading; empty if none
};

struct CompiledApp {
    uint64_t generation = 0;
    RouteTable routes;
    StaticIndex statics;
    TemplateSet templates;
    std::map<RelPath, MarkdownPage> markdownPages;
    // frags/layout/markdown.html, which wraps every Markdown page, if the
    // app has one (with {{content}} and {{title}}); else a built-in page.
    std::optional<CompiledTemplate> markdownLayout;
    // frags/auth/layout.html, which wraps the engine's sign-in pages (with
    // {{content}}, the engine's form or message, and {{title}}), if the app
    // has one; else a built-in page.
    std::optional<CompiledTemplate> authLayout;
    // pages/markdown.css exists, so pages with Markdown link it instead of
    // the built-in stylesheet.
    bool hasMarkdownCss = false;
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
