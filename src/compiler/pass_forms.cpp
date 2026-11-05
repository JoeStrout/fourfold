// The forms pass (overview §4, Forms and validation; "Checking forms
// against templates").
//
// 1. Each page file's form declarations (form.sign = {...}), found when its
//    top level ran at compile time, become FormSpecs: on the route of the
//    post handler with the same name, and in CompiledApp::forms for
//    formState(). Mistakes in a declaration are errors.
// 2. Each <form> in a template that posts to a route is checked against
//    the route's declaration: an input the declaration doesn't name (which
//    would be dropped), or a declared field with no input, is a warning. The
//    declaration's rules become browser-side validation attributes
//    (required, maxlength, minlength, min, max, accept) on the inputs that
//    don't already have them. And a form posting to a route with a
//    challenge (code/limits.ms) is marked to carry a proof-of-work puzzle.
//
// After the routes, access and limits passes.

#include <algorithm>
#include <cmath>
#include <cctype>
#include <set>
#include <tuple>

#include "links.h"
#include "passes.h"

namespace fourfold {

namespace {

const char* const kRules[] = {"required", "min", "max", "maxLinks", "options", "maxKB", "types", "type"};

bool isFieldName(const std::string& name) {
    if (name.empty() || name.size() > 64 || !std::isalpha(static_cast<unsigned char>(name[0]))) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool isTextType(FormFieldType t) {
    return t == FormFieldType::Text || t == FormFieldType::Multiline || t == FormFieldType::Email ||
           t == FormFieldType::Url;
}

std::string show(double n) {
    if (n == std::floor(n) && std::fabs(n) < 1e15) return std::to_string(static_cast<long long>(n));
    std::string s = std::to_string(n);
    while (!s.empty() && s.back() == '0') s.pop_back();
    return s;
}

// One field's declaration; errors go to `fail`.
std::optional<FormField> readField(const std::string& name, const Datum& value,
                                   const std::function<void(const std::string&)>& fail) {
    if (!isFieldName(name)) {
        fail("\"" + name + "\" can't be a field name: use a letter, then letters, digits, _ or -");
        return std::nullopt;
    }
    if (name == "errors") {
        fail("a field can't be named \"errors\": formState() and the form's fragment use that name for the errors");
        return std::nullopt;
    }
    if (value.kind() != Datum::Kind::Map) {
        fail(name + " must be a map, like {\"type\": \"text\", \"required\": true}");
        return std::nullopt;
    }
    const Datum::MapItems& m = value.asMap();
    FormField f;
    f.name = name;
    auto type = m.find("type");
    if (type == m.end() || type->second.kind() != Datum::Kind::String) {
        fail(name + " needs a \"type\": text, multiline, email, url, number, int, bool, choice, choices, date or "
                    "file");
        return std::nullopt;
    }
    auto t = formFieldTypeFrom(type->second.asString());
    if (!t) {
        fail(name + ": \"" + type->second.asString() + "\" isn't a field type (text, multiline, email, url, number, "
                    "int, bool, choice, choices, date or file)");
        return std::nullopt;
    }
    f.type = *t;
    bool ok = true;
    auto bad = [&](const std::string& message) {
        fail(name + "." + message);
        ok = false;
    };
    bool numeric = f.type == FormFieldType::Number || f.type == FormFieldType::Int;
    for (const auto& [key, v] : m) {
        if (key == "type") continue;
        if (key == "required") {
            if (v.kind() != Datum::Kind::Number || (v.asNumber() != 0 && v.asNumber() != 1)) {
                bad("required must be true or false");
            } else {
                f.required = v.asNumber() == 1;
            }
        } else if (key == "min" || key == "max") {
            if (!isTextType(f.type) && !numeric) {
                bad(key + " applies to text (its length) and numbers (their value), not to a " +
                    formFieldTypeName(f.type) + " field");
            } else if (v.kind() != Datum::Kind::Number) {
                bad(key + " must be a number");
            } else if (isTextType(f.type) && (v.asNumber() < 0 || v.asNumber() != std::floor(v.asNumber()))) {
                bad(key + " is a length here: a whole number, 0 or more");
            } else {
                (key == "min" ? f.min : f.max) = v.asNumber();
            }
        } else if (key == "maxLinks") {
            if (!isTextType(f.type)) {
                bad("maxLinks applies to text fields");
            } else if (v.kind() != Datum::Kind::Number || v.asNumber() < 0 || v.asNumber() != std::floor(v.asNumber())) {
                bad("maxLinks must be a whole number, 0 or more");
            } else {
                f.maxLinks = static_cast<int>(v.asNumber());
            }
        } else if (key == "options") {
            if (f.type != FormFieldType::Choice && f.type != FormFieldType::Choices) {
                bad("options applies to choice and choices fields");
            } else if (v.kind() != Datum::Kind::List || v.asList().empty()) {
                bad("options must be a list of strings, like [\"happy\", \"meh\"]");
            } else {
                for (const Datum& o : v.asList()) {
                    if (o.kind() != Datum::Kind::String || o.asString().empty()) {
                        bad("options must be a list of non-empty strings");
                        break;
                    }
                    if (std::find(f.options.begin(), f.options.end(), o.asString()) != f.options.end()) {
                        bad("options has \"" + o.asString() + "\" twice");
                        break;
                    }
                    f.options.push_back(o.asString());
                }
            }
        } else if (key == "maxKB") {
            if (f.type != FormFieldType::File) {
                bad("maxKB applies to file fields");
            } else if (v.kind() != Datum::Kind::Number || !(v.asNumber() > 0 && v.asNumber() <= 16 * 1024)) {
                bad("maxKB must be a number of kilobytes, more than 0 and at most 16384 (16 MB)");
            } else {
                f.maxKB = v.asNumber();
            }
        } else if (key == "types") {
            if (f.type != FormFieldType::File) {
                bad("types applies to file fields");
            } else if (v.kind() != Datum::Kind::List || v.asList().empty()) {
                bad("types must be a list of content types, like [\"image/png\", \"image/jpeg\"]");
            } else {
                for (const Datum& o : v.asList()) {
                    std::string declared = o.kind() == Datum::Kind::String ? o.asString() : "";
                    if (!isCheckableUploadType(declared)) {
                        bad("types: \"" + declared + "\" can't be checked by content; the types are image/png, "
                                                 "image/jpeg, image/gif, image/webp, image/* (all four), "
                                                 "application/pdf, application/zip and text/plain");
                        break;
                    }
                    f.types.push_back(declared);
                }
            }
        } else if (key == "messages") {
            if (v.kind() != Datum::Kind::Map) {
                bad("messages must be a map from rules to messages, like {\"required\": \"Please sign your name\"}");
                continue;
            }
            for (const auto& [rule, message] : v.asMap()) {
                bool known = false;
                for (const char* r : kRules) known = known || rule == r;
                if (!known) {
                    bad("messages has \"" + rule + "\", which isn't a rule (required, min, max, maxLinks, "
                                                   "options, maxKB, types, or type for a value of the wrong kind)");
                } else if (message.kind() != Datum::Kind::String) {
                    bad("messages." + rule + " must be a string");
                } else {
                    f.messages[rule] = message.asString();
                }
            }
        } else {
            bad(key + " isn't a field setting (type, required, min, max, maxLinks, options, maxKB, types, "
                      "messages)");
        }
    }
    if ((f.type == FormFieldType::Choice || f.type == FormFieldType::Choices) && f.options.empty() && ok) {
        bad("options is needed for a " + std::string(formFieldTypeName(f.type)) + " field");
    }
    if (f.type == FormFieldType::File && ok) {
        if (f.types.empty()) bad("types is needed for a file field: uploads are checked by content against it");
        if (f.maxKB == 0) f.maxKB = 1024;
    }
    if (f.min && f.max && *f.min > *f.max && ok) bad("min is more than max");
    if (!ok) return std::nullopt;
    return f;
}

// "pages/guestbook.ms" → "pages/guestbook" (to compare with page files).
std::string stemPath(const RelPath& path) {
    std::string s = path.str();
    s.resize(s.size() - path.extension().size());
    return s;
}

// The URL of the page that goes with page file `source`: the route whose
// render it has, or whose .html or .md is beside it.
std::string pageUrlFor(const CompiledApp& app, const RelPath& source) {
    std::string stem = stemPath(source);
    for (const auto& [url, entry] : app.routes) {
        if (entry.kind != RouteKind::Page && entry.kind != RouteKind::Markdown) continue;
        bool same = (entry.render && entry.render->source == source) || stemPath(entry.source) == stem;
        if (!same) continue;
        return entry.trailingSlash && url != "/" ? url + "/" : url;
    }
    return "";
}

// The validation attributes for one control, from its field: only those
// the markup doesn't already have.
std::string attributesFor(const CompiledTemplate::Form::Control& c, const FormField& f) {
    auto has = [&](const char* name) {
        return std::find(c.attributes.begin(), c.attributes.end(), name) != c.attributes.end();
    };
    std::string out;
    auto add = [&](const char* name, const std::string& value) {
        if (has(name)) return;
        out += " ";
        out += name;
        if (!value.empty()) out += "=\"" + value + "\"";
    };
    bool checkbox = c.type == "checkbox";
    if (f.required && f.type != FormFieldType::Choices && !(checkbox && f.type != FormFieldType::Bool) &&
        c.element != "button" && c.type != "hidden" && c.type != "submit") {
        add("required", "");
    }
    if (isTextType(f.type) && (c.element == "textarea" || c.element == "input")) {
        if (f.max) add("maxlength", show(*f.max));
        if (f.min && *f.min > 0) add("minlength", show(*f.min));
    }
    if ((f.type == FormFieldType::Number || f.type == FormFieldType::Int) && c.element == "input") {
        if (f.min) add("min", show(*f.min));
        if (f.max) add("max", show(*f.max));
        if (f.type == FormFieldType::Int && c.type == "number") add("step", "1");
    }
    if (f.type == FormFieldType::File && c.element == "input" && c.type == "file") {
        std::string accept;
        for (const std::string& t : f.types) accept += (accept.empty() ? "" : ",") + t;
        add("accept", accept);
    }
    return out;
}

struct FormChecker {
    CompiledApp& app;
    Diagnostics& diagnostics;
    std::set<std::tuple<std::string, int, std::string>> warned;   // file, line, message: once, however included

    void warn(const std::string& file, int line, const std::string& message) {
        if (warned.insert({file, line, message}).second) diagnostics.warning("code/" + file, message, line);
    }

    void check(CompiledTemplate& t, const std::optional<std::vector<std::string>>& folder) {
        t.annotateForms([&](const CompiledTemplate::Form& form) {
            CompiledTemplate::FormAnnotation a;
            a.controlAttributes.resize(form.controls.size());
            if (form.url.empty()) return a;
            bool above = false;
            std::optional<LocalUrl> local = localUrl(folder, form.url, above);
            if (!local) return a;
            const RouteEntry* route = app.routes.find(local->path);
            if (!route) return a;   // the links pass reports it
            a.challenge = route->challenge;
            const FormSpec* spec = route->form.get();
            if (!spec) return a;
            std::set<std::string> seen;
            for (size_t i = 0; i < form.controls.size(); i++) {
                const auto& c = form.controls[i];
                if (!c.name.empty() && c.name[0] == '_') continue;
                seen.insert(c.name);
                const FormField* f = spec->find(c.name);
                if (!f) {
                    warn(c.file, c.line, "<" + c.element + " name=\"" + c.name + "\"> posts to " + local->path +
                                             ", whose " + spec->label() + " (code/" + spec->source.str() +
                                             ") doesn't declare it, so it would be dropped");
                    continue;
                }
                a.controlAttributes[i] = attributesFor(c, *f);
            }
            for (const FormField& f : spec->fields) {
                if (!seen.count(f.name)) {
                    warn(form.file, form.line, "this form posts to " + local->path + ", whose " + spec->label() +
                                                   " declares \"" + f.name + "\", but it has no input named \"" +
                                                   f.name + "\"");
                }
            }
            return a;
        });
    }
};

std::optional<std::vector<std::string>> folderOf(const RelPath& source) {
    std::vector<std::string> folder;
    RelPath parent = source.parent();
    std::vector<std::string_view> segments = parent.segments();
    for (size_t i = 1; i < segments.size(); i++) folder.emplace_back(segments[i]);
    return folder;
}

}  // namespace

void formsPass(const std::map<RelPath, PageHandlers>& handlers, CompiledApp& app, Diagnostics& diagnostics) {
    // 1. The declarations.
    for (const auto& [source, h] : handlers) {
        std::string fileName = codeFileName(source);
        for (const auto& [name, value] : h.forms) {
            std::string label = "form." + name;
            if (std::find(h.post.begin(), h.post.end(), name) == h.post.end()) {
                diagnostics.error(fileName, label + " declares a form for post." + name +
                                                ", but there's no post." + name + " in this file");
                continue;
            }
            if (value.kind() != Datum::Kind::Map || value.asMap().empty()) {
                diagnostics.error(fileName, label + " must be a map from field names to their declarations, like "
                                                    "{\"who\": {\"type\": \"text\", \"required\": true}}");
                continue;
            }
            auto spec = std::make_shared<FormSpec>();
            spec->source = source;
            spec->name = name;
            bool ok = true;
            for (const auto& [field, declaration] : value.asMap()) {
                auto f = readField(field, declaration, [&](const std::string& message) {
                    diagnostics.error(fileName, label + "." + message);
                    ok = false;
                });
                if (f) spec->fields.push_back(std::move(*f));
            }
            if (!ok) continue;
            app.forms[spec->key()] = spec;
        }
    }

    // ...on the post handlers' routes, with their pages' URLs.
    for (const auto& [url, original] : app.routes) {
        if (!original.post) continue;
        auto spec = app.forms.find(original.post->source.str() + "#" + original.post->name);
        if (spec == app.forms.end()) continue;
        RouteEntry* entry = app.routes.findForUpdate(url);
        entry->form = spec->second;
        entry->pageUrl = pageUrlFor(app, original.post->source);
        // reshow() keeps the visitor's input in the session.
        entry->usesSession = true;
    }

    // 2. The forms in templates.
    FormChecker checker{app, diagnostics, {}};
    for (const auto& [url, route] : app.routes) {
        if (route.kind != RouteKind::Page) continue;
        if (CompiledTemplate* t = app.templates.findForUpdate(route.source)) checker.check(*t, folderOf(route.source));
    }
    for (const std::optional<RelPath>* special : {&app.notFoundPage, &app.errorPage, &app.tooManyPage}) {
        if (!*special) continue;
        if (CompiledTemplate* t = app.templates.findForUpdate(**special)) checker.check(*t, std::nullopt);
    }
    for (auto& [name, t] : app.templates.fragmentsForUpdate()) checker.check(t, std::nullopt);
    if (app.markdownLayout) checker.check(*app.markdownLayout, std::nullopt);
    if (app.authLayout) checker.check(*app.authLayout, std::nullopt);
}

}  // namespace fourfold
