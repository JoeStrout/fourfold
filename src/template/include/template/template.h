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
// Markdown files in code/ are templates too (Dynamic Markdown, M6.1): the
// markdown component parses them (parseMarkdownTemplate), into an HTML form
// and a Markdown form with the same fields, and they're rendered here, in
// either format.
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
    // In Markdown templates. (UrlStart, UrlPart and Attribute are used there
    // too: a link's destination, and an image's alt text or a link's title.)
    MarkdownText,    // *{{x}}*: text, escaped for the output; a rendered value is an error
    MarkdownBlock,   // {{x}} alone on a line, at the top level: a string is a
                     // paragraph; a rendered Markdown value is its own blocks
};

// What a render writes: HTML, or Markdown (a Markdown template's Markdown
// form, for /about.md).
enum class OutputFormat { Html, Markdown };

// The Content-Type of a Markdown response.
extern const char* const kMarkdownContentType;   // "text/markdown; charset=utf-8; variant=GFM"

const char* fieldContextName(FieldContext context);

struct TemplateField {
    std::vector<std::string> path;   // {"user", "name"}
    FieldContext context = FieldContext::Text;
    std::string file;                // where the field is written: "pages/about.html"
    int line = 0;
    std::string attribute;           // the attribute's name, in attribute contexts
    // Markdown output only: the text after the field starts with '.', so a
    // value ending in "www" would make a web address there (see
    // escapeMarkdownText).
    bool followedByDot = false;

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
            PostForm,    // just after a <form method="post"> start tag: the CSRF token field
            BodyStart,   // before the end of the <body> start tag: hx-headers with the CSRF token
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

    const RelPath& source() const { return source_; }
    TemplateKind kind() const { return kind_; }
    // The HTML form.
    const std::vector<Part>& parts() const { return parts_; }
    // A Markdown template's Markdown form: its source, split at its fields
    // (and with its includes in place). Empty for an HTML template.
    const std::vector<Part>& markdownParts() const { return markdownParts_; }
    // A Markdown page's title: the text of its first # heading, as Literal
    // parts (plain text, not HTML) and Field parts.
    const std::vector<Part>& titleParts() const { return titleParts_; }
    // Each top-level field name, once: the template's own in order of first
    // use, then those of included fragments.
    const std::vector<std::string>& fieldNames() const { return fieldNames_; }
    bool hasFields() const { return !fieldNames_.empty(); }
    bool hasScript() const { return hasScript_; }
    bool hasMarkdown() const { return hasMarkdown_; }
    // A Markdown template (.md), with both forms.
    bool isMarkdown() const { return isMarkdown_; }
    size_t postForms() const { return postForms_; }
    // Its literal links, including those of included fragments.
    const std::vector<Link>& links() const { return links_; }

private:
    friend struct TemplateParser;
    friend struct IncludeResolver;
    friend struct MarkdownTemplateBuilder;   // in the markdown component

    RelPath source_;
    TemplateKind kind_ = TemplateKind::Fragment;
    std::vector<Part> parts_;
    std::vector<Part> markdownParts_;
    std::vector<Part> titleParts_;
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
// is an error naming the chain; a fragment in one is left unresolved. A
// Markdown template includes only Markdown fragments; an HTML one includes
// either (a Markdown fragment's HTML form).
std::vector<TemplateProblem> resolveIncludes(FragmentMap& fragments);

// Resolves a page's includes against fragments already resolved.
std::vector<TemplateProblem> resolveIncludes(CompiledTemplate& page, const FragmentMap& fragments);

// --- Rendering ---

struct RenderOptions {
    // HTML, or Markdown: only Markdown templates have a Markdown form.
    OutputFormat format = OutputFormat::Html;
    // The response's CSP nonce, added to every <script>. Empty adds none.
    std::string nonce;
    // The stylesheet linked at the end of <head> when the output contains
    // rendered Markdown. Empty links none.
    std::string markdownCss;
    // A Markdown page's /x.md URL, linked at the end of <head> in its HTML
    // output (rel="alternate"). Empty links none.
    std::string markdownAlternate;
    // Report missing and unused fields. Off for pages with no code, whose
    // fields render empty on purpose (overview §4).
    bool warnings = true;
    // The visitor's CSRF token, written into every <form method="post"> as
    // a hidden _csrf field, and into <body> as HTMX's X-CSRF-Token header
    // (overview §5, CSRF protection). Empty writes none.
    std::string csrfToken;
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
    Html html;              // the HTML output
    std::string markdown;   // the Markdown output, when the format is Markdown
    std::vector<RenderWarning> warnings;
    std::optional<RenderError> error;   // set if the render failed; html is then empty

    bool ok() const { return !error; }
};

// Renders the template in options.format. A Markdown output from an HTML
// template is an error.
RenderResult render(const CompiledTemplate& compiled, const FieldSource& fields,
                    const RenderOptions& options = {});

// A fragment, as frag() returns it, in `html`. A Markdown fragment's result
// is Markdown, with its Markdown form rendered too when options.format is
// Markdown (otherwise left unmade: the run's output is HTML).
RenderResult renderFragment(const CompiledTemplate& compiled, const FieldSource& fields,
                            const RenderOptions& options = {});

// Nothing, rendered from `compiled`: what frag(name, []) returns. It's
// Markdown if the fragment is.
Html emptyRendering(const CompiledTemplate& compiled, const RenderOptions& options = {});

// A Markdown page's title, with its fields filled: what the layout's
// {{title}} shows. Empty if it has no # heading.
std::string pageTitle(const CompiledTemplate& page, const FieldSource& fields);

// The page a Markdown page's HTML output is when the app has no
// frags/layout/markdown.html: a minimal document around `content`.
std::string builtinMarkdownPage(const std::string& title, const Html& content, const RenderOptions& options);

// A Markdown page in options.format: as HTML, its HTML form wrapped in
// `layout` (frags/layout/markdown.html, filling {{content}} and {{title}};
// or the built-in page if null); as Markdown, its Markdown form alone.
RenderResult renderMarkdownPage(const CompiledTemplate& page, const CompiledTemplate* layout,
                                const FieldSource& fields, const RenderOptions& options = {});

}  // namespace fourfold
