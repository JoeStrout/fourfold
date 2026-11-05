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

const char* formFieldTypeName(FormFieldType type) {
    switch (type) {
        case FormFieldType::Text: return "text";
        case FormFieldType::Multiline: return "multiline";
        case FormFieldType::Email: return "email";
        case FormFieldType::Url: return "url";
        case FormFieldType::Number: return "number";
        case FormFieldType::Int: return "int";
        case FormFieldType::Bool: return "bool";
        case FormFieldType::Choice: return "choice";
        case FormFieldType::Choices: return "choices";
        case FormFieldType::Date: return "date";
        case FormFieldType::File: return "file";
    }
    return "text";
}

std::optional<FormFieldType> formFieldTypeFrom(const std::string& name) {
    for (FormFieldType t : {FormFieldType::Text, FormFieldType::Multiline, FormFieldType::Email, FormFieldType::Url,
                            FormFieldType::Number, FormFieldType::Int, FormFieldType::Bool, FormFieldType::Choice,
                            FormFieldType::Choices, FormFieldType::Date, FormFieldType::File}) {
        if (name == formFieldTypeName(t)) return t;
    }
    return std::nullopt;
}

const FormField* FormSpec::find(const std::string& field) const {
    for (const FormField& f : fields) {
        if (f.name == field) return &f;
    }
    return nullptr;
}

bool FormSpec::hasFiles() const {
    for (const FormField& f : fields) {
        if (f.type == FormFieldType::File) return true;
    }
    return false;
}

size_t FormSpec::maxUploadBytes() const {
    double total = 0;
    for (const FormField& f : fields) {
        if (f.type == FormFieldType::File) total += f.maxKB * 1024;
    }
    return static_cast<size_t>(total);
}

bool isCheckableUploadType(const std::string& type) {
    static const char* const kTypes[] = {"image/png",       "image/jpeg",      "image/gif",  "image/webp",
                                         "application/pdf", "application/zip", "text/plain", "image/*"};
    for (const char* t : kTypes) {
        if (type == t) return true;
    }
    return false;
}

namespace {

std::string perName(RateRule::Per per) {
    switch (per) {
        case RateRule::Per::Ip: return "ip";
        case RateRule::Per::User: return "user";
        case RateRule::Per::Session: return "session";
    }
    return "ip";
}

}  // namespace

std::string RateRule::describe() const {
    std::string unit = seconds == 1 ? "second" : seconds == 60 ? "minute" : seconds == 3600 ? "hour"
                                               : seconds == 86400 ? "day" : std::to_string(seconds) + " seconds";
    return std::to_string(count) + "/" + unit + " per " + perName(per);
}

std::optional<RateRule> parseRate(const std::string& text, std::string& error) {
    // "/" and " per " mean the same; split into words.
    std::string spaced;
    for (char c : text) {
        if (c == '/') {
            spaced += " per ";
        } else {
            spaced += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    std::vector<std::string> words;
    std::string word;
    for (char c : spaced + " ") {
        if (c == ' ' || c == '\t') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    const std::string help = "write a rate like \"5/minute\", \"100/hour per user\" or \"10/day per session\"";
    if (words.size() != 3 && words.size() != 5) {
        error = "\"" + text + "\" isn't a rate; " + help;
        return std::nullopt;
    }
    RateRule r;
    const std::string& n = words[0];
    bool digits = !n.empty() && n.size() <= 9;
    for (char c : n) digits = digits && std::isdigit(static_cast<unsigned char>(c));
    if (!digits || std::stoll(n) < 1) {
        error = "\"" + text + "\": the count must be a whole number from 1 up; " + help;
        return std::nullopt;
    }
    r.count = std::stoll(n);
    if (words[1] != "per") {
        error = "\"" + text + "\" isn't a rate; " + help;
        return std::nullopt;
    }
    std::string unit = words[2];
    if (unit.size() > 1 && unit.back() == 's') unit.pop_back();
    if (unit == "second") r.seconds = 1;
    else if (unit == "minute") r.seconds = 60;
    else if (unit == "hour") r.seconds = 3600;
    else if (unit == "day") r.seconds = 86400;
    else {
        error = "\"" + text + "\": the unit must be second, minute, hour or day";
        return std::nullopt;
    }
    if (words.size() == 5) {
        if (words[3] != "per") {
            error = "\"" + text + "\" isn't a rate; " + help;
            return std::nullopt;
        }
        if (words[4] == "ip") r.per = RateRule::Per::Ip;
        else if (words[4] == "user") r.per = RateRule::Per::User;
        else if (words[4] == "session") r.per = RateRule::Per::Session;
        else {
            error = "\"" + text + "\": counted per ip, user or session, not \"" + words[4] + "\"";
            return std::nullopt;
        }
    }
    return r;
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
    if (aliases_.count(path)) return false;
    return routes_.emplace(std::move(path), std::move(entry)).second;
}

const RouteEntry* RouteTable::find(const std::string& path) const {
    auto it = routes_.find(path);
    return it == routes_.end() ? nullptr : &it->second;
}

RouteEntry* RouteTable::findForUpdate(const std::string& path) {
    auto it = routes_.find(path);
    return it == routes_.end() ? nullptr : &it->second;
}

bool RouteTable::addMarkdownAlias(std::string alias, std::string path) {
    if (routes_.count(alias) || !routes_.count(path)) return false;
    return aliases_.emplace(std::move(alias), std::move(path)).second;
}

const RouteEntry* RouteTable::findMarkdown(const std::string& alias, std::string* path) const {
    auto it = aliases_.find(alias);
    if (it == aliases_.end()) return nullptr;
    if (path) *path = it->second;
    return find(it->second);
}

std::string markdownUrlFor(const std::string& path, bool trailingSlash) {
    if (path == "/") return "/index.md";
    return path + (trailingSlash ? "/index.md" : ".md");
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

CompiledTemplate* TemplateSet::findForUpdate(const RelPath& source) {
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
        if (entry.kind == RouteKind::Markdown) s += "  " + markdownUrlFor(path, entry.trailingSlash);
        if (entry.kind == RouteKind::Page || entry.kind == RouteKind::Markdown) {
            if (const CompiledTemplate* t = app.templates.find(entry.source)) {
                if (t->hasFields()) {
                    s += "  fields:";
                    for (const std::string& f : t->fieldNames()) s += " " + f;
                }
                std::string title;
                for (const CompiledTemplate::Part& p : t->titleParts()) {
                    title += p.kind == CompiledTemplate::Part::Kind::Field ? "{{" + p.field.label() + "}}" : p.text;
                }
                if (!title.empty()) s += "  title: " + title;
            }
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
        if (!entry.limits.empty()) {
            s += "  limits:";
            for (const RateRule& r : entry.limits) s += " " + r.describe() + ";";
            s.pop_back();
        }
        if (entry.minSeconds > 0) s += "  minSeconds: " + std::to_string(static_cast<int64_t>(entry.minSeconds));
        if (entry.challenge) s += "  challenge";
        if (entry.form) {
            s += "  " + entry.form->label() + ":";
            for (const FormField& f : entry.form->fields) {
                s += " " + f.name + "(" + formFieldTypeName(f.type) + (f.required ? ", required" : "") + ")";
            }
        }
        if (!entry.pageUrl.empty() && entry.form) s += "  page: " + entry.pageUrl;
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
    if (app.tooManyPage) s += "too many requests page: " + app.tooManyPage->str() + "\n";
    s += "diagnostics:\n";
    for (const Diagnostic& d : app.diagnostics) s += "  " + d.toString() + "\n";
    return s;
}

}  // namespace fourfold
