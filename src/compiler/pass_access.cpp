// The access pass: each route's effective access level (overview §5, Access
// control; "Access is resolved per route, at compile time").
//
// Two sources, both evaluated as data:
//
//   code/access.ms     rules = {}; rules["/admin"] = "admin"; ...
//                      A rule covers its path and everything below it, on
//                      whole segments: "/admin" covers /admin/users, not
//                      /administrator. No wildcards.
//   access = "login"   in a page's .ms file: the page and every handler in
//                      that file.
//
// Every rule that applies must be met (AND); a list within one rule is any
// of its roles (OR). Adding a rule anywhere can only tighten access. A rule
// in access.ms that covers no route is a warning: the usual sign of a
// renamed folder whose rule silently stopped applying.

#include "passes.h"

namespace fourfold {

namespace {

const char* const kAccessFile = "access.ms";
const char* const kValueHelp =
    "\"login\", a role name like \"admin\", or a list of role names like [\"admin\", \"moderator\"]";

// One rule from access.ms.
struct Rule {
    std::string path;                 // "/admin"
    std::vector<std::string> anyOf;   // empty: "login"
    bool used = false;
};

// Whether `rule` ("/admin") covers the route at `url` ("/admin/users").
bool covers(const std::string& rule, const std::string& url) {
    if (rule == "/") return true;
    return url == rule || (url.size() > rule.size() && url.compare(0, rule.size(), rule) == 0 && url[rule.size()] == '/');
}

// "pages/admin.html" → "pages/admin.ms".
RelPath codeFor(const RelPath& path) {
    std::string s = path.str();
    s.resize(s.size() - path.extension().size());
    return RelPath::parse(s + ".ms").value();
}

// The .ms file whose `access =` applies to a route: the one its code is in,
// or, for a page with no render, the one beside its .html.
std::optional<RelPath> codeFileOf(const RouteEntry& entry) {
    for (const std::optional<HandlerRef>* h : {&entry.render, &entry.get, &entry.post}) {
        if (*h) return (*h)->source;
    }
    if (entry.kind == RouteKind::Page) return codeFor(entry.source);
    return std::nullopt;
}

}  // namespace

std::optional<std::vector<std::string>> parseAccessValue(const Datum& value, std::string& error) {
    if (value.kind() == Datum::Kind::String) {
        const std::string& s = value.asString();
        if (s == "login") return std::vector<std::string>();
        if (!isValidRoleName(s)) {
            error = "\"" + s + "\" isn't a role name (a letter, then letters, digits, _ or -); use " + kValueHelp;
            return std::nullopt;
        }
        return std::vector<std::string>{s};
    }
    if (value.kind() == Datum::Kind::List && !value.asList().empty()) {
        std::vector<std::string> roles;
        for (const Datum& item : value.asList()) {
            if (item.kind() != Datum::Kind::String || !isValidRoleName(item.asString())) {
                std::string shown = item.kind() == Datum::Kind::String ? "\"" + item.asString() + "\"" : toJson(item);
                error = "the list has " + shown + ", which isn't a role name" +
                        (shown == "\"login\"" ? " (\"login\" goes on its own, not in a list)" : "");
                return std::nullopt;
            }
            roles.push_back(item.asString());
        }
        return roles;
    }
    error = "it's " + toJson(value) + "; use " + kValueHelp;
    return std::nullopt;
}

void accessPass(const CodeListing& listing, const CodeTree& code, const std::map<RelPath, PageHandlers>& handlers,
                DataEvaluator* evaluator, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath accessPath = RelPath::parse(kAccessFile).value();
    const std::string accessName = codeFileName(accessPath);
    std::vector<Rule> rules;

    bool hasFile = false;
    for (const CodeEntry& e : listing.files) hasFile = hasFile || e.path == accessPath;
    if (hasFile) {
        auto file = code.read(accessPath);
        if (!file) {
            diagnostics.error(accessName, file.error().message);
        } else if (!evaluator) {
            diagnostics.error(accessName, "can't be read: no evaluator was provided");
        } else {
            EvaluatedData d = evaluator->evaluate(file.value(), "rules");
            if (d.error) {
                diagnostics.error(d.error->file.empty() ? accessName : d.error->file, d.error->message, d.error->line);
            } else if (d.value.kind() != Datum::Kind::Map) {
                diagnostics.error(accessName, d.value.kind() == Datum::Kind::Null
                                                  ? "must set `rules`, a map from paths to access levels "
                                                    "(start with rules = {})"
                                                  : "`rules` must be a map from paths to access levels");
            } else {
                for (const auto& [path, value] : d.value.asMap()) {
                    std::string where = "rules[\"" + path + "\"]: ";
                    auto canonical = canonicalUrlPath(path);
                    if (!canonical || canonical.value().path != path || path.find('*') != std::string::npos ||
                        path.find('?') != std::string::npos || path.find('#') != std::string::npos) {
                        std::string why = path.find('*') != std::string::npos
                                              ? "paths have no wildcards; a rule covers everything below its path"
                                          : !path.empty() && path.back() == '/' && path != "/"
                                              ? "write the path without the trailing slash"
                                              : "not a plain URL path like \"/admin\"";
                        diagnostics.error(accessName, where + why);
                        continue;
                    }
                    std::string error;
                    auto anyOf = parseAccessValue(value, error);
                    if (!anyOf) {
                        diagnostics.error(accessName, where + error);
                        continue;
                    }
                    rules.push_back({path, std::move(*anyOf)});
                }
            }
        }
    }

    // Each page file's own declaration, checked once.
    std::map<RelPath, std::vector<std::string>> declared;
    for (const auto& [path, h] : handlers) {
        if (!h.access) continue;
        std::string error;
        auto anyOf = parseAccessValue(*h.access, error);
        if (!anyOf) {
            diagnostics.error(codeFileName(path), "access: " + error);
            continue;
        }
        declared.emplace(path, std::move(*anyOf));
    }

    RouteTable routes;
    for (const auto& [url, original] : app.routes) {
        RouteEntry entry = original;
        for (Rule& rule : rules) {
            if (!covers(rule.path, url)) continue;
            rule.used = true;
            entry.access.require(rule.anyOf);
            entry.accessSources.push_back(accessName + ": " + rule.path);
        }
        if (auto source = codeFileOf(entry)) {
            auto d = declared.find(*source);
            if (d != declared.end()) {
                entry.access.require(d->second);
                entry.accessSources.push_back(codeFileName(*source));
            }
        }
        routes.add(url, std::move(entry));
    }
    app.routes = std::move(routes);

    for (const Rule& rule : rules) {
        if (!rule.used) {
            diagnostics.warning(accessName, "the rule for " + rule.path +
                                                " covers no page, handler or file (was a folder renamed?)");
        }
    }
}

}  // namespace fourfold
