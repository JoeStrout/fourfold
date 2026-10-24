#include "compiledapp/compiled_app.h"

namespace fourfold {

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
        for (const std::optional<HandlerRef>* h : {&entry.render, &entry.get, &entry.post}) {
            if (*h) s += "  " + (*h)->label() + " in " + (*h)->source.str();
        }
        s += "\n";
    }
    s += "not found page: " + (app.notFoundPage ? app.notFoundPage->str() : std::string("(none)")) + "\n";
    s += "error page: " + (app.errorPage ? app.errorPage->str() : std::string("(none)")) + "\n";
    s += "diagnostics:\n";
    for (const Diagnostic& d : app.diagnostics) s += "  " + d.toString() + "\n";
    return s;
}

}  // namespace fourfold
