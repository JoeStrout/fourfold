// Rendering: each field's value, escaped for its context (overview §4, "How
// values become text").

#include <algorithm>
#include <cstdlib>
#include <set>

#include "template/template.h"

namespace fourfold {

namespace {

struct Renderer {
    const CompiledTemplate& compiled;
    const FieldSource& fields;
    const RenderOptions& options;

    RenderResult result;
    std::string out;
    bool hasScript = false;
    bool hasMarkdown = false;
    std::set<std::string> reportedMissing;

    void warn(const TemplateField& f, std::string message) {
        if (!options.warnings) return;
        RenderWarning w{f.file, f.line, std::move(message)};
        for (const RenderWarning& existing : result.warnings) {
            if (existing == w) return;
        }
        result.warnings.push_back(std::move(w));
    }

    // Records the error; returns false, so callers can `return fail(...)`.
    bool fail(const TemplateField& f, const std::string& message) {
        result.error = RenderError{f.file, f.line, "{{" + f.label() + "}} " + message};
        return false;
    }

    static std::string where(const TemplateField& f) {
        switch (f.context) {
            case FieldContext::Rcdata: return "inside <title> or <textarea>";
            case FieldContext::Script: return "in a <script>";
            default: return "in an attribute (" + f.attribute + ")";
        }
    }

    // The value as it's written in the field's context, into `text`.
    bool write(const TemplateField& f, const FieldValue& v, std::string& text) {
        switch (v.kind) {
            case FieldValue::Kind::Error:
                return fail(f, v.text);
            case FieldValue::Kind::Composite:
                return fail(f, "is " + v.text + "; a field shows a string or a number (render a map with frag(), "
                                "or pass it to a <script> field)");
            case FieldValue::Kind::Html:
                if (f.context != FieldContext::Text) {
                    return fail(f, "is rendered HTML, which can only go in text, not " + where(f));
                }
                hasScript = hasScript || v.html.hasScript();
                hasMarkdown = hasMarkdown || v.html.hasMarkdown();
                text = v.html.str();
                return true;
            case FieldValue::Kind::Missing:
            case FieldValue::Kind::Null:
                text = f.context == FieldContext::Script ? "null" : "";
                return true;
            case FieldValue::Kind::Data:
                if (f.context != FieldContext::Script) {
                    return fail(f, "is a value meant for a <script>");
                }
                text = toJsonForScript(v.data);
                return true;
            case FieldValue::Kind::Number:
            case FieldValue::Kind::String:
                break;
        }
        switch (f.context) {
            case FieldContext::Text:
            case FieldContext::Rcdata:
            case FieldContext::Attribute:
                text = escapeHtml(v.text);
                return true;
            case FieldContext::UrlStart: {
                std::optional<std::string> url = filterUrl(v.text);
                if (!url) {
                    warn(f, "{{" + f.label() + "}} isn't a safe URL (only http, https, mailto and relative URLs are "
                            "allowed), so it was replaced with " + kInvalidUrl);
                    url = kInvalidUrl;
                }
                text = escapeHtml(*url);
                return true;
            }
            case FieldContext::UrlPart:
                text = percentEncode(v.text);
                return true;
            case FieldContext::Script:
                text = toJsonForScript(v.kind == FieldValue::Kind::Number
                                           ? Datum::number(std::strtod(v.text.c_str(), nullptr))
                                           : Datum::string(v.text));
                return true;
            case FieldContext::Boolean:
                break;
        }
        return fail(f, "can't be written here");
    }

    FieldValue lookup(const TemplateField& f) {
        FieldValue v = fields.lookup(f.path, f.context == FieldContext::Script);
        if (v.kind == FieldValue::Kind::Missing && reportedMissing.insert(f.path[0]).second) {
            warn(f, "{{" + f.path[0] + "}} wasn't given a value, so it's empty");
        }
        return v;
    }

    static bool truthy(const FieldValue& v) {
        switch (v.kind) {
            case FieldValue::Kind::Number: return std::strtod(v.text.c_str(), nullptr) != 0;
            case FieldValue::Kind::String: return !v.text.empty();
            default: return false;
        }
    }

    bool part(const CompiledTemplate::Part& p, size_t& headAt) {
        using Kind = CompiledTemplate::Part::Kind;
        switch (p.kind) {
            case Kind::Literal:
                out += p.text;
                return true;
            case Kind::Nonce:
                if (!options.nonce.empty()) out += " nonce=\"" + escapeHtml(options.nonce) + "\"";
                return true;
            case Kind::HeadEnd:
                headAt = out.size();
                return true;
            case Kind::PostForm:
            case Kind::Include:
                return true;
            case Kind::Field: {
                std::string text;
                if (!write(p.field, lookup(p.field), text)) return false;
                out += text;
                return true;
            }
            case Kind::Attribute: {
                FieldValue v = lookup(p.field);
                if (v.kind == FieldValue::Kind::Missing || v.kind == FieldValue::Kind::Null) return true;
                if (p.field.context == FieldContext::Boolean) {
                    if (v.kind == FieldValue::Kind::Error) return fail(p.field, v.text);
                    if (v.kind != FieldValue::Kind::Number && v.kind != FieldValue::Kind::String) {
                        return fail(p.field, "is a boolean attribute (" + p.field.attribute +
                                                 "), so it takes a number, a string or null");
                    }
                    if (truthy(v)) out += p.text;
                    return true;
                }
                std::string text;
                if (!write(p.field, v, text)) return false;
                out += p.text + p.between + text + p.after;
                return true;
            }
        }
        return true;
    }

    void run() {
        hasScript = compiled.hasScript();
        hasMarkdown = compiled.hasMarkdown();
        size_t headAt = std::string::npos;
        for (const CompiledTemplate::Part& p : compiled.parts()) {
            if (!part(p, headAt)) {
                result.html = Html();
                return;
            }
        }
        if (options.warnings) {
            const std::vector<std::string>& used = compiled.fieldNames();
            for (const std::string& name : fields.names()) {
                if (std::find(used.begin(), used.end(), name) != used.end()) continue;
                result.warnings.push_back({compiled.source().str(), 0,
                                           "\"" + name + "\" was given a value, but code/" +
                                               compiled.source().str() + " has no {{" + name + "}} field"});
            }
        }
        if (hasMarkdown && !options.markdownCss.empty() && headAt != std::string::npos) {
            out.insert(headAt, "<link rel=\"stylesheet\" href=\"" + escapeHtml(options.markdownCss) + "\">\n");
        }
        result.html = HtmlAccess::make(std::move(out), hasScript, hasMarkdown);
    }
};

}  // namespace

RenderResult render(const CompiledTemplate& compiled, const FieldSource& fields, const RenderOptions& options) {
    Renderer r{compiled, fields, options, {}, {}, false, false, {}};
    r.run();
    return std::move(r.result);
}

}  // namespace fourfold
