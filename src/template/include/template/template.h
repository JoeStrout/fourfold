// Templates: HTML with {{fields}} (overview §4, "Template syntax and safety").
//
// Every template is parsed at compile time, with a tokenizer that follows
// the HTML spec's, and each field is classified by where it is: text, a
// quoted attribute, a URL, a <script>, and so on. A field anywhere unsafe or
// ambiguous is a compile error, so a bad template never deploys. Rendering
// then escapes each value for its field's context (Go's html/template is the
// model). docs/design/m4-results.md lists every rule and why.
//
// - {{name}}, {{ user.name }}: a field, with an optional dotted path.
// - \{{ : a literal "{{", anywhere.
// - {{> header}}: frags/header.html (or .md), pasted in at compile time.
//
// Templates are parsed only from CodeFile, so only from code/ (Arch §7).
// Nothing here depends on MiniScript: values come through a FieldSource.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "partition/code_tree.h"
#include "partition/paths.h"
#include "template/escape.h"
#include "template/field_source.h"
#include "template/html.h"

namespace fourfold {

// A page (in pages/) must be a complete document; a fragment (in frags/)
// must not be one.
enum class TemplateKind { Page, Fragment };

// Where a field is, which decides how its value is written.
enum class FieldContext {
    Text,        // <p>{{x}}</p>: escaped; Html inserted as is
    Rcdata,      // <title>, <textarea>: escaped
    Attribute,   // title="{{x}}": escaped
    UrlStart,    // href="{{x}}": the URL rule, then escaped
    UrlPart,     // href="/a?b={{x}}": percent-encoded
    Script,      // <script> const c = {{x}};: a JSON literal
    Boolean,     // checked="{{x}}": the attribute is there if x is truthy
};

const char* fieldContextName(FieldContext context);

struct TemplateField {
    std::vector<std::string> path;   // {"user", "name"}
    FieldContext context = FieldContext::Text;
    std::string file;                // where the field is written: "pages/about.html"
    int line = 0;
    std::string attribute;           // the attribute's name, in attribute contexts

    std::string label() const;       // "user.name"
};

class CompiledTemplate {
public:
    struct Part {
        enum class Kind {
            Literal,     // `text`, as is
            Field,       // `field`
            // An attribute whose whole value is one field: `text` is the
            // attribute up to its value (` title`), `between` is `="`, and
            // `after` the closing quote. A null value leaves it all out; a
            // boolean attribute writes only `text`, or nothing.
            Attribute,
            Nonce,       // after "<script": nonce="…", if the render has one
            HeadEnd,     // where markdown.css is linked, if the page needs it
            PostForm,    // just after a <form method="post"> start tag (CSRF, M5)
            Include,     // {{> text}}, until includes are resolved
        };

        Kind kind = Kind::Literal;
        std::string text;
        std::string between;
        std::string after;
        TemplateField field;
        int line = 0;   // Include
    };

    // A URL written literally (with no fields) in a link attribute: href,
    // src, action, formaction, poster, <object data>, or HTMX's hx-get and
    // the like. The compiler checks the local ones against the routes.
    struct Link {
        std::string url;         // as written, with &amp; decoded
        std::string attribute;   // "href"
        std::string file;        // where it's written: "frags/nav.html"
        int line = 0;
    };

    // A fragment made from rendered Markdown (frags/intro.md).
    static CompiledTemplate fromMarkdown(RelPath source, Html html);

    const RelPath& source() const { return source_; }
    TemplateKind kind() const { return kind_; }
    const std::vector<Part>& parts() const { return parts_; }
    // Each top-level field name, once: the template's own in order of first
    // use, then those of included fragments.
    const std::vector<std::string>& fieldNames() const { return fieldNames_; }
    bool hasFields() const { return !fieldNames_.empty(); }
    bool hasScript() const { return hasScript_; }
    bool hasMarkdown() const { return hasMarkdown_; }
    // A .md fragment: it has no fields, so it takes no value.
    bool isMarkdown() const { return isMarkdown_; }
    size_t postForms() const { return postForms_; }
    // Its literal links, including those of included fragments.
    const std::vector<Link>& links() const { return links_; }

private:
    friend struct TemplateParser;
    friend struct IncludeResolver;

    RelPath source_;
    TemplateKind kind_ = TemplateKind::Fragment;
    std::vector<Part> parts_;
    std::vector<std::string> fieldNames_;
    bool hasScript_ = false;
    bool hasMarkdown_ = false;
    bool isMarkdown_ = false;
    size_t postForms_ = 0;
    std::vector<Link> links_;
};

struct TemplateProblem {
    std::string file;   // "pages/about.html", relative to code/
    int line = 0;
    std::string message;
};

struct ParsedTemplate {
    CompiledTemplate compiled;            // complete only if there are no errors
    std::vector<TemplateProblem> errors;

    bool ok() const { return errors.empty(); }
};

ParsedTemplate parseTemplate(const CodeFile& file, TemplateKind kind);

// --- Includes ---

// Fragments by name: "header" for frags/header.html, "auth/login" for
// frags/auth/login.html.
using FragmentMap = std::map<std::string, CompiledTemplate>;

// Resolves the includes inside the fragments themselves, in place. A cycle
// is an error naming the chain; a fragment in one is left unresolved.
std::vector<TemplateProblem> resolveIncludes(FragmentMap& fragments);

// Resolves a page's includes against fragments already resolved.
std::vector<TemplateProblem> resolveIncludes(CompiledTemplate& page, const FragmentMap& fragments);

// --- Rendering ---

struct RenderOptions {
    // The response's CSP nonce, added to every <script>. Empty adds none.
    std::string nonce;
    // The stylesheet linked at the end of <head> when the output contains
    // rendered Markdown. Empty links none.
    std::string markdownCss;
    // Report missing and unused fields. Off for pages with no code, whose
    // fields render empty on purpose (overview §4).
    bool warnings = true;
};

struct RenderWarning {
    std::string file;   // relative to code/
    int line = 0;
    std::string message;

    bool operator==(const RenderWarning& o) const {
        return file == o.file && line == o.line && message == o.message;
    }
};

struct RenderError {
    std::string file;
    int line = 0;
    std::string message;
};

struct RenderResult {
    Html html;
    std::vector<RenderWarning> warnings;
    std::optional<RenderError> error;   // set if the render failed; html is then empty

    bool ok() const { return !error; }
};

RenderResult render(const CompiledTemplate& compiled, const FieldSource& fields,
                    const RenderOptions& options = {});

}  // namespace fourfold
