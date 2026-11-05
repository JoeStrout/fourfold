// Rendering: each field's value, escaped for its context (overview §4, "How
// values become text").

#include <algorithm>
#include <cstdlib>
#include <set>

#include "template/template.h"

namespace fourfold {

const char* const kMarkdownContentType = "text/markdown; charset=utf-8; variant=GFM";

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

    bool markdownOutput() const { return options.format == OutputFormat::Markdown; }

    // A field in a Markdown template.
    bool writeMarkdownField(const TemplateField& f, const FieldValue& v, std::string& text) {
        bool block = f.context == FieldContext::MarkdownBlock;
        switch (v.kind) {
            case FieldValue::Kind::Error:
                return fail(f, v.text);
            case FieldValue::Kind::Composite:
                return fail(f, "is " + v.text + "; a field shows a string or a number (render a map with frag())");
            case FieldValue::Kind::Data:
                return fail(f, "is a value meant for a <script>");
            case FieldValue::Kind::Html: {
                if (!block) {
                    return fail(f, "is a rendered value (from frag() or markdown()), which in a Markdown template must "
                                   "be a paragraph of its own: alone on its line, at the top level (not in a list or "
                                   "quote), with a blank line before and after it");
                }
                if (!v.html.isMarkdown()) {
                    return fail(f, "is HTML (from an HTML fragment or unsafeHTML), but code/" + compiled.source().str() +
                                   " is Markdown, which takes only Markdown: an .md fragment, or markdown()");
                }
                if (markdownOutput()) {
                    const std::string* md = v.html.markdown();
                    if (!md) return fail(f, "has no Markdown form (it was rendered for an HTML page)");
                    // Its own blocks: a blank line before (the field starts a
                    // line), and its last line ended by the field's own.
                    text.clear();
                    size_t end = md->find_last_not_of('\n');
                    if (end != std::string::npos) {
                        bool blank = out.empty() || (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0);
                        text = (blank ? "" : "\n") + md->substr(0, end + 1);
                    }
                } else {
                    hasScript = hasScript || v.html.hasScript();
                    hasMarkdown = hasMarkdown || v.html.hasMarkdown();
                    text = v.html.str();
                }
                return true;
            }
            case FieldValue::Kind::Missing:
            case FieldValue::Kind::Null:
                text.clear();
                return true;
            case FieldValue::Kind::Number:
            case FieldValue::Kind::String:
                break;
        }
        if (markdownOutput()) {
            switch (f.context) {
                case FieldContext::UrlStart: {
                    std::optional<std::string> url = filterUrl(v.text);
                    if (!url) {
                        warn(f, "{{" + f.label() + "}} isn't a safe URL (only http, https, mailto and relative URLs "
                                "are allowed), so it was replaced with " + kInvalidUrl);
                        url = kInvalidUrl;
                    }
                    text = markdownDestination(*url);
                    return true;
                }
                case FieldContext::UrlPart:
                    text = percentEncode(v.text);
                    return true;
                default:
                    // Text, alt text, a title, or a paragraph of its own.
                    text = escapeMarkdownText(v.text, f.followedByDot);
                    return true;
            }
        }
        if (block) {
            text = v.text.empty() ? "" : "<p>" + escapeHtml(v.text) + "</p>\n";
        } else {
            text = escapeHtml(v.text);
        }
        return true;
    }

    // The value as it's written in the field's context, into `text`.
    bool write(const TemplateField& f, const FieldValue& v, std::string& text) {
        // A Markdown template's fields: all of them in its Markdown form, and
        // its text and block fields in its HTML form. (Its URL, alt-text and
        // title fields are written into HTML as an HTML template's are.)
        if (markdownOutput() || f.context == FieldContext::MarkdownText || f.context == FieldContext::MarkdownBlock) {
            return writeMarkdownField(f, v, text);
        }
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
            case FieldContext::MarkdownText:
            case FieldContext::MarkdownBlock:
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
                if (!options.csrfToken.empty()) {
                    out += "<input type=\"hidden\" name=\"_csrf\" value=\"" + escapeHtml(options.csrfToken) + "\">";
                }
                return true;
            case Kind::BodyStart:
                if (!options.csrfToken.empty()) {
                    out += " hx-headers=\"" + escapeHtml("{\"X-CSRF-Token\": \"" + options.csrfToken + "\"}") + "\"";
                }
                return true;
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
        if (markdownOutput() && !compiled.isMarkdown()) {
            result.error = RenderError{compiled.source().str(), 0,
                                       "is HTML, so it has no Markdown form; only Markdown templates do"};
            return;
        }
        for (const CompiledTemplate::Part& p : markdownOutput() ? compiled.markdownParts() : compiled.parts()) {
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
        if (markdownOutput()) {
            result.markdown = std::move(out);
            return;
        }
        if (headAt != std::string::npos) {
            std::string links;
            if (hasMarkdown && !options.markdownCss.empty()) {
                links += "<link rel=\"stylesheet\" href=\"" + escapeHtml(options.markdownCss) + "\">\n";
            }
            if (!options.markdownAlternate.empty()) {
                links += "<link rel=\"alternate\" type=\"text/markdown\" href=\"" +
                         escapeHtml(options.markdownAlternate) + "\">\n";
            }
            out.insert(headAt, links);
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

RenderResult renderFragment(const CompiledTemplate& compiled, const FieldSource& fields,
                            const RenderOptions& options) {
    RenderOptions asHtml = options;
    asHtml.format = OutputFormat::Html;
    RenderResult r = render(compiled, fields, asHtml);
    if (!r.ok() || !compiled.isMarkdown()) return r;
    std::shared_ptr<const MarkdownForm> form = MarkdownForm::notMade();
    if (options.format == OutputFormat::Markdown) {
        RenderResult md = render(compiled, fields, options);
        for (RenderWarning& w : md.warnings) {
            if (std::find(r.warnings.begin(), r.warnings.end(), w) == r.warnings.end()) r.warnings.push_back(std::move(w));
        }
        if (!md.ok()) {
            md.warnings = std::move(r.warnings);
            return md;
        }
        form = MarkdownForm::ready(std::move(md.markdown));
    }
    r.html = HtmlAccess::make(r.html.str(), r.html.hasScript(), r.html.hasMarkdown(), std::move(form));
    return r;
}

Html emptyRendering(const CompiledTemplate& compiled, const RenderOptions& options) {
    if (!compiled.isMarkdown()) return Html();
    return HtmlAccess::make("", false, false,
                            options.format == OutputFormat::Markdown ? MarkdownForm::ready("") : MarkdownForm::notMade());
}

std::string pageTitle(const CompiledTemplate& page, const FieldSource& fields) {
    std::string title;
    for (const CompiledTemplate::Part& p : page.titleParts()) {
        if (p.kind == CompiledTemplate::Part::Kind::Literal) {
            title += p.text;
        } else if (p.kind == CompiledTemplate::Part::Kind::Field) {
            FieldValue v = fields.lookup(p.field.path, false);
            if (v.kind == FieldValue::Kind::String || v.kind == FieldValue::Kind::Number) title += v.text;
        }
    }
    return title;
}

std::string builtinMarkdownPage(const std::string& title, const Html& content, const RenderOptions& options) {
    std::string links;
    if (!options.markdownCss.empty()) {
        links += "<link rel=\"stylesheet\" href=\"" + escapeHtml(options.markdownCss) + "\">\n";
    }
    if (!options.markdownAlternate.empty()) {
        links += "<link rel=\"alternate\" type=\"text/markdown\" href=\"" + escapeHtml(options.markdownAlternate) +
                 "\">\n";
    }
    return "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
           escapeHtml(title) + "</title>\n" + links +
           "<style>body{max-width:46rem;margin:2rem auto;padding:0 1rem;"
           "font-family:system-ui,sans-serif}</style>\n</head>\n<body>\n" +
           content.str() + "</body></html>\n";
}

RenderResult renderMarkdownPage(const CompiledTemplate& page, const CompiledTemplate* layout,
                                const FieldSource& fields, const RenderOptions& options) {
    if (options.format == OutputFormat::Markdown) return render(page, fields, options);
    RenderResult content = render(page, fields, options);
    if (!content.ok()) return content;
    std::string title = pageTitle(page, fields);
    if (!layout) {
        content.html = HtmlAccess::make(builtinMarkdownPage(title, content.html, options), content.html.hasScript(),
                                        true);
        return content;
    }
    MapFields wrap;
    wrap.set("content", FieldValue::markup(content.html));
    wrap.set("title", title.empty() ? FieldValue::null() : FieldValue::string(title));
    RenderOptions quiet = options;
    quiet.warnings = false;   // the layout's fields are the engine's to fill
    RenderResult r = render(*layout, wrap, quiet);
    r.warnings.insert(r.warnings.begin(), content.warnings.begin(), content.warnings.end());
    return r;
}

}  // namespace fourfold
