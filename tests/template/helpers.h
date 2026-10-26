// Helpers for the template tests. Templates come only from CodeFile, so the
// tests go through a real code tree, in one scratch folder per process.
#pragma once

#include <memory>
#include <string>

#include "doctest.h"
#include "partition/app_folder.h"
#include "template/template.h"
#include "temp_dir.h"

namespace fourfold::test {

inline CodeFile codeFile(const std::string& path, const std::string& text) {
    static TempDir dir;
    static std::unique_ptr<CodeTree> code;
    dir.write("code/" + path, text);
    if (!code) {
        auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create);
        REQUIRE(app.ok());
        auto tree = app.value().codeTree();
        REQUIRE(tree.ok());
        code = std::make_unique<CodeTree>(std::move(tree.value()));
    }
    auto file = code->read(RelPath::parse(path).value());
    REQUIRE(file.ok());
    return std::move(file.value());
}

inline ParsedTemplate parseFragment(const std::string& text) {
    return parseTemplate(codeFile("frags/t.html", text), TemplateKind::Fragment);
}

inline ParsedTemplate parsePage(const std::string& text) {
    return parseTemplate(codeFile("pages/t.html", text), TemplateKind::Page);
}

// The first error's message, or "" if it parsed.
inline std::string fragmentError(const std::string& text) {
    ParsedTemplate p = parseFragment(text);
    return p.ok() ? "" : p.errors[0].message;
}

inline std::string pageError(const std::string& text) {
    ParsedTemplate p = parsePage(text);
    return p.ok() ? "" : p.errors[0].message;
}

// Renders a fragment, which must parse; "ERROR: message" if rendering fails.
inline std::string renderFragment(const std::string& text, const FieldSource& fields = EmptyFields(),
                                  const RenderOptions& options = {}) {
    ParsedTemplate p = parseFragment(text);
    INFO("template: ", text);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.errors[0].message));
    RenderResult r = render(p.compiled, fields, options);
    if (!r.ok()) return "ERROR: " + r.error->message;
    return r.html.str();
}

inline MapFields one(const std::string& name, FieldValue value) {
    MapFields f;
    f.set(name, std::move(value));
    return f;
}

inline MapFields oneString(const std::string& name, const std::string& value) {
    return one(name, FieldValue::string(value));
}

}  // namespace fourfold::test
