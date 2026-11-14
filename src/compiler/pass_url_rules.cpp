// The URL rules pass (M11.2): redirects and rewrites for URLs no route
// serves, from code/routes.ms, evaluated as data:
//
//   redirects = {}
//   redirects["/wiki/{rest*}"] = "https://wiki.miniscript.org/wiki/{rest}"
//   redirects["/old-news"] = {"to": "/news/", "status": 302}
//
//   rewrites = {}
//   rewrites["/tryit/{code}"] = "/tryit/?s={code}"
//   rewrites["/favicon.ico"] = "/img/favicon/favicon.ico"
//
// Redirects are checked first, then rewrites, each in the order written; the
// first that matches wins. A rule only ever sees URLs no page, handler or
// file serves. A redirect's status is 301 unless it says 302, 307 or 308. A
// rewrite serves one fixed route of the app (checked here), with
// placeholders only in its query; the visitor's URL stays in the address
// bar.

#include "passes.h"

namespace fourfold {

namespace {

const char* const kRoutesFile = "routes.ms";

// The folder a URL's relative links resolve against: "/tryit/" for both
// "/tryit/x" and "/tryit/".
std::string folderOf(const std::string& url) {
    size_t slash = url.rfind('/');
    return url.substr(0, slash + 1);
}

struct Reader {
    const CompiledApp& app;
    Diagnostics& diagnostics;
    std::string fileName;
    std::vector<UrlRule> rules;

    void error(const std::string& where, const std::string& message) { diagnostics.error(fileName, where + message); }
    void warning(const std::string& where, const std::string& message) {
        diagnostics.warning(fileName, where + message);
    }

    // The route at a canonical path, or a Markdown URL's route; `markdown`
    // says which.
    const RouteEntry* routeAt(const std::string& path, bool& markdown) const {
        markdown = false;
        if (const RouteEntry* e = app.routes.find(path)) return e;
        markdown = true;
        return app.routes.findMarkdown(path);
    }

    void read(const Datum& map, UrlRule::Kind kind, const char* global) {
        for (const auto& [key, value] : map.asMap()) {
            std::string where = std::string(global) + "[\"" + key + "\"]: ";
            std::string why;
            std::optional<UrlPattern> pattern = UrlPattern::parse(key, why);
            if (!pattern) {
                error(where, why);
                continue;
            }
            UrlRule rule;
            rule.kind = kind;
            rule.source = fileName + ": " + global + "[\"" + key + "\"]";
            std::string targetText;
            if (value.kind() == Datum::Kind::String) {
                targetText = value.asString();
            } else if (kind == UrlRule::Kind::Redirect && value.kind() == Datum::Kind::Map) {
                bool ok = true;
                for (const auto& [name, v] : value.asMap()) {
                    if (name == "to" && v.kind() == Datum::Kind::String) {
                        targetText = v.asString();
                    } else if (name == "status" && v.kind() == Datum::Kind::Number &&
                               (v.asNumber() == 301 || v.asNumber() == 302 || v.asNumber() == 307 ||
                                v.asNumber() == 308)) {
                        rule.status = static_cast<int>(v.asNumber());
                    } else if (name == "status") {
                        error(where, "status must be 301, 302, 307 or 308");
                        ok = false;
                    } else if (name == "to") {
                        error(where, "\"to\" must be a URL");
                        ok = false;
                    } else {
                        error(where, "unknown setting \"" + name + "\" (a redirect's map has to and status)");
                        ok = false;
                    }
                }
                if (!ok) continue;
                if (targetText.empty()) {
                    error(where, "the map needs \"to\", the URL to redirect to");
                    continue;
                }
            } else {
                error(where, kind == UrlRule::Kind::Redirect
                                 ? "a redirect is a URL, or a map like {\"to\": \"/new\", \"status\": 302}"
                                 : "a rewrite is a URL on this site, like \"/tryit/?s={code}\"");
                continue;
            }
            std::optional<UrlTarget> target = UrlTarget::parse(targetText, *pattern, why);
            if (!target) {
                error(where, "\"" + targetText + "\": " + why);
                continue;
            }
            rule.pattern = std::move(*pattern);
            rule.target = std::move(*target);
            if (kind == UrlRule::Kind::Rewrite && !checkRewrite(rule, where)) continue;

            // Rules only see URLs no route serves.
            if (!rule.pattern.hasPlaceholders()) {
                bool markdown = false;
                auto path = canonicalUrlPath(rule.pattern.text());
                if (path && routeAt(path.value().path, markdown)) {
                    const RouteEntry* e = routeAt(path.value().path, markdown);
                    warning(where, "never applies: " + path.value().path + " is served by " +
                                       codeFileName(e->source) + ", and a route always wins over a rule");
                }
            }
            for (const UrlRule& earlier : rules) {
                if (rule.pattern.coveredBy(earlier.pattern)) {
                    warning(where, "never reached: " + earlier.source.substr(fileName.size() + 2) +
                                       " comes first and matches every URL this does" +
                                       (earlier.kind != rule.kind ? " (redirects are checked before rewrites)" : ""));
                    break;
                }
            }
            rules.push_back(std::move(rule));
        }
    }

    bool checkRewrite(UrlRule& rule, const std::string& where) {
        const UrlTarget& target = rule.target;
        if (target.kind() != UrlTarget::Kind::Local) {
            error(where, "a rewrite serves a URL on this site; to send visitors elsewhere, use a redirect");
            return false;
        }
        if (target.hasPathPlaceholders()) {
            error(where, "a rewrite serves one page, file or handler, so its path can't have placeholders; put them "
                         "in its query (\"/tryit/?s={code}\"), or use a redirect");
            return false;
        }
        std::optional<UrlPath> path = target.fixedPath();
        if (!path) {
            error(where, "\"" + target.text() + "\" isn't a valid URL path");
            return false;
        }
        bool markdown = false;
        const RouteEntry* entry = routeAt(path->path, markdown);
        if (!entry) {
            error(where, "no page, handler or file is served at " + path->path +
                             " (a rewrite's target must be; rewrites aren't followed again)");
            return false;
        }
        bool slash = entry->trailingSlash && !markdown;
        if (slash != path->trailingSlash) {
            std::string canonical = encodeUrlPath(path->path) + (slash ? "/" : "");
            error(where, "the URL of " + codeFileName(entry->source) + " is " + canonical + "; write that");
            return false;
        }
        if (entry->kind == RouteKind::Handler && !entry->get) {
            error(where, path->path + " has no get handler, and rules apply to GET only");
            return false;
        }
        rule.routePath = path->path;
        rule.markdownAlias = markdown;
        if (entry->kind == RouteKind::Page || entry->kind == RouteKind::Markdown) {
            std::string pageFolder = folderOf(encodeUrlPath(path->path) + (slash ? "/" : ""));
            std::string ruleFolder = folderOf(rule.pattern.text());
            if (ruleFolder != pageFolder) {
                warning(where, "the page's relative links resolve against " + pageFolder + ", but the visitor's URL "
                               "is in " + ruleFolder + ", so they'd break; use absolute links in it, or a redirect");
            }
        }
        return true;
    }

    // After every rule is read: a local redirect to a URL nothing serves.
    void checkRedirectTargets() {
        for (const UrlRule& rule : rules) {
            if (rule.kind != UrlRule::Kind::Redirect) continue;
            std::optional<UrlPath> path = rule.target.fixedPath();
            if (!path) continue;
            bool markdown = false;
            if (routeAt(path->path, markdown)) continue;
            std::string where = rule.source.substr(fileName.size() + 2) + ": ";
            UrlValues values;
            if (rule.pattern.match(*path, values)) {
                error(where, "redirects to " + path->path + ", which it matches itself: a loop");
                continue;
            }
            bool ruled = false;
            for (const UrlRule& other : rules) ruled = ruled || other.pattern.match(*path, values);
            if (!ruled) warning(where, "redirects to " + path->path + ", which nothing serves");
        }
    }
};

}  // namespace

void urlRulesPass(const CodeListing& listing, const CodeTree& code, DataEvaluator* evaluator, CompiledApp& app,
                  Diagnostics& diagnostics) {
    const RelPath path = RelPath::parse(kRoutesFile).value();
    const std::string fileName = codeFileName(path);
    bool hasFile = false;
    for (const CodeEntry& e : listing.files) hasFile = hasFile || e.path == path;
    if (!hasFile) return;

    auto file = code.read(path);
    if (!file) {
        diagnostics.error(fileName, file.error().message);
        return;
    }
    if (!evaluator) {
        diagnostics.error(fileName, "can't be read: no evaluator was provided");
        return;
    }
    Reader reader{app, diagnostics, fileName, {}};
    bool any = false;
    for (const char* global : {"redirects", "rewrites"}) {
        EvaluatedData d = evaluator->evaluate(file.value(), global);
        if (d.error) {
            diagnostics.error(d.error->file.empty() ? fileName : d.error->file, d.error->message, d.error->line);
            return;
        }
        if (d.value.isNull()) continue;
        any = true;
        if (d.value.kind() != Datum::Kind::Map) {
            diagnostics.error(fileName, std::string("`") + global + "` must be a map from URL patterns to URLs");
            continue;
        }
        reader.read(d.value, std::string(global) == "redirects" ? UrlRule::Kind::Redirect : UrlRule::Kind::Rewrite,
                    global);
    }
    if (!any) {
        diagnostics.error(fileName, "must set `redirects` or `rewrites` (or both), maps from URL patterns to URLs "
                                    "(start with redirects = {})");
        return;
    }
    reader.checkRedirectTargets();
    app.urlRules = std::move(reader.rules);
}

}  // namespace fourfold
