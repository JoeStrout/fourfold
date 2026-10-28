#include "compiledapp/compiled_app.h"

#include <algorithm>
#include <cctype>

namespace fourfold {

namespace {

// Whether requirement `a` implies `b`: every visitor meeting `a` meets `b`.
// Empty means "login", which every requirement implies.
bool implies(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (b.empty()) return true;
    if (a.empty()) return false;
    for (const std::string& role : a) {
        if (!std::binary_search(b.begin(), b.end(), role)) return false;
    }
    return true;
}

}  // namespace

void AccessLevel::require(std::vector<std::string> anyOf) {
    std::sort(anyOf.begin(), anyOf.end());
    anyOf.erase(std::unique(anyOf.begin(), anyOf.end()), anyOf.end());
    for (const auto& existing : all) {
        if (implies(existing, anyOf)) return;
    }
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const std::vector<std::string>& existing) { return implies(anyOf, existing); }),
              all.end());
    all.push_back(std::move(anyOf));
    std::sort(all.begin(), all.end());
}

bool AccessLevel::allows(bool signedIn, const std::vector<std::string>& roles) const {
    if (all.empty()) return true;
    if (!signedIn) return false;
    for (const auto& anyOf : all) {
        if (anyOf.empty()) continue;
        bool met = false;
        for (const std::string& role : roles) met = met || std::binary_search(anyOf.begin(), anyOf.end(), role);
        if (!met) return false;
    }
    return true;
}

std::string AccessLevel::describe() const {
    if (all.empty()) return "public";
    std::string out;
    for (const auto& anyOf : all) {
        if (!out.empty()) out += " and ";
        if (anyOf.empty()) {
            out += "login";
            continue;
        }
        std::string roles;
        for (const std::string& role : anyOf) roles += (roles.empty() ? "" : " or ") + role;
        out += anyOf.size() > 1 && all.size() > 1 ? "(" + roles + ")" : roles;
    }
    return out;
}

bool isValidRoleName(const std::string& role) {
    if (role.empty() || role.size() > 64 || !std::isalpha(static_cast<unsigned char>(role[0]))) return false;
    for (char c : role) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return role != "login" && role != "public";
}

std::string Diagnostic::toString() const {
    std::string s = file;
    if (line > 0) s += ":" + std::to_string(line);
    s += severity == Severity::Error ? ": error: " : ": warning: ";
    s += message;
    return s;
}

const char* routeKindName(RouteKind kind) {
    switch (kind) {
        case RouteKind::Page: return "page";
        case RouteKind::Static: return "static";
        case RouteKind::Handler: return "handler";
        case RouteKind::Markdown: return "markdown";
    }
    return "unknown";
}

std::string HandlerRef::label() const {
    switch (kind) {
        case Kind::Render: return "render";
        case Kind::Get: return "get." + name;
        case Kind::Post: return "post." + name;
    }
    return name;
}

bool RouteTable::add(std::string path, RouteEntry entry) {
    return routes_.emplace(std::move(path), std::move(entry)).second;
}

const RouteEntry* RouteTable::find(const std::string& path) const {
    auto it = routes_.find(path);
    return it == routes_.end() ? nullptr : &it->second;
}

void StaticIndex::add(RelPath source, StaticFile file) {
    files_[std::move(source)] = std::move(file);
}

const StaticFile* StaticIndex::find(const RelPath& source) const {
    auto it = files_.find(source);
    return it == files_.end() ? nullptr : &it->second;
}

void TemplateSet::add(RelPath source, CompiledTemplate compiled) {
    templates_[std::move(source)] = std::move(compiled);
}

const CompiledTemplate* TemplateSet::find(const RelPath& source) const {
    auto it = templates_.find(source);
    return it == templates_.end() ? nullptr : &it->second;
}

const CompiledTemplate* TemplateSet::fragment(const std::string& name) const {
    auto it = fragments_.find(name);
    return it == fragments_.end() ? nullptr : &it->second;
}

std::string describe(const CompiledApp& app) {
    std::string s = "routes:\n";
    for (const auto& [path, entry] : app.routes) {
        s += "  " + path + (entry.trailingSlash ? "/" : "") + "  " + routeKindName(entry.kind) + "  " +
             entry.source.str();
        if (entry.kind == RouteKind::Static) {
            if (const StaticFile* f = app.statics.find(entry.source)) s += "  " + f->contentType;
        }
        if (entry.kind == RouteKind::Page) {
            if (const CompiledTemplate* t = app.templates.find(entry.source)) {
                if (t->hasFields()) {
                    s += "  fields:";
                    for (const std::string& f : t->fieldNames()) s += " " + f;
                }
            }
        }
        if (entry.kind == RouteKind::Markdown) {
            auto md = app.markdownPages.find(entry.source);
            if (md != app.markdownPages.end() && !md->second.title.empty()) s += "  title: " + md->second.title;
        }
        for (const std::optional<HandlerRef>* h : {&entry.render, &entry.get, &entry.post}) {
            if (*h) s += "  " + (*h)->label() + " in " + (*h)->source.str();
        }
        if (entry.usesSession) s += "  session";
        if (!entry.access.isPublic()) {
            s += "  access: " + entry.access.describe() + " (";
            for (size_t i = 0; i < entry.accessSources.size(); i++) s += (i ? "; " : "") + entry.accessSources[i];
            s += ")";
        }
        s += "\n";
    }
    if (!app.templates.fragments().empty()) {
        s += "fragments:\n";
        for (const auto& [name, t] : app.templates.fragments()) {
            s += "  " + name + "  " + t.source().str();
            if (t.hasFields()) {
                s += "  fields:";
                for (const std::string& f : t.fieldNames()) s += " " + f;
            }
            s += "\n";
        }
    }
    if (app.markdownLayout) s += "markdown layout: " + app.markdownLayout->source().str() + "\n";
    if (app.authLayout) s += "sign-in layout: " + app.authLayout->source().str() + "\n";
    s += "not found page: " + (app.notFoundPage ? app.notFoundPage->str() : std::string("(none)")) + "\n";
    s += "error page: " + (app.errorPage ? app.errorPage->str() : std::string("(none)")) + "\n";
    s += "diagnostics:\n";
    for (const Diagnostic& d : app.diagnostics) s += "  " + d.toString() + "\n";
    return s;
}

}  // namespace fourfold
