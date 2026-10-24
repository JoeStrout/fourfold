// Templates: HTML with {{fields}} (overview §4).
//
// This is M2's temporary, minimal version, replaced by the full contextual
// template engine in M4. It allows only what it can make safe without that
// engine:
//
// - A field is {{name}} (spaces inside optional), where name is an
//   identifier. No dotted paths or includes yet.
// - Fields may appear only in text: not inside a tag (so not in attributes),
//   nor in a comment, <script>, <style>, <textarea>, <title>, or the other
//   raw-text elements. Anywhere else, {{ is a compile error, so no template
//   can rely on behavior that M4 will change.
// - \{{ is a literal "{{", anywhere.
// - Values are HTML-escaped. A missing field renders as empty.
//
// Templates are parsed only from CodeFile, so only from code/ (Arch §7).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "partition/code_tree.h"

namespace fourfold {

class CompiledTemplate {
public:
    // Literal text and fields, in order.
    struct Part {
        bool isField = false;
        std::string text;   // the literal text, or the field's name
    };

    const std::vector<Part>& parts() const { return parts_; }
    // Each field's name, once, in order of first use.
    const std::vector<std::string>& fieldNames() const { return fieldNames_; }
    bool hasFields() const { return !fieldNames_.empty(); }

private:
    friend struct TemplateParser;
    std::vector<Part> parts_;
    std::vector<std::string> fieldNames_;
};

struct TemplateProblem {
    int line = 0;
    std::string message;
};

struct ParsedTemplate {
    CompiledTemplate compiled;            // complete only if there are no errors
    std::vector<TemplateProblem> errors;

    bool ok() const { return errors.empty(); }
};

ParsedTemplate parseTemplate(const CodeFile& file);

// Field values, already formatted as text (the script bridge formats numbers
// the MiniScript way). Null, or a missing name, renders as empty.
using TemplateFields = std::map<std::string, std::optional<std::string>>;

std::string renderTemplate(const CompiledTemplate& compiled, const TemplateFields& fields);

// &, <, >, ", and ' as character references.
std::string escapeHtml(const std::string& text);

}  // namespace fourfold
