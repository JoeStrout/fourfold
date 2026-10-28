// The templates pass: every template is parsed once, at compile time, so a
// template that breaks the field rules never deploys.
//
//   frags/**/*.html   fragments, by name ("entry", "auth/login")
//   frags/**/*.md     Markdown fragments, rendered now (no fields)
//   frags/layout/markdown.html   the wrapper for Markdown pages: a whole document
//   frags/auth/layout.html       the wrapper for the engine's sign-in pages: a whole document
//   pages/**/*.html   pages
//
// Fragments' includes are resolved first, then the pages'.

#include <algorithm>

#include "markdown/markdown.h"
#include "passes.h"

namespace fourfold {

namespace {

const char* const kMarkdownLayout = "layout/markdown";
const char* const kAuthLayout = "auth/layout";

void report(const std::vector<TemplateProblem>& problems, Diagnostics& diagnostics) {
    for (const TemplateProblem& p : problems) {
        diagnostics.error(p.file.empty() ? "code" : "code/" + p.file, p.message, p.line);
    }
}

// "frags/auth/login.html" → "auth/login".
std::string fragmentName(const RelPath& path) {
    std::string s = path.str().substr(6);   // past "frags/"
    s.resize(s.size() - path.extension().size());
    return s;
}

}  // namespace

void templatesPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    const RelPath frags = RelPath::parse("frags").value();

    FragmentMap fragments;
    std::map<std::string, RelPath> fragmentSources;   // name → the file it came from
    std::optional<CompiledTemplate> layout, authLayout;

    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(frags)) continue;
        std::string extension = lowerExtension(entry.path);
        std::string fileName = codeFileName(entry.path);
        if (extension != ".html" && extension != ".md") {
            diagnostics.warning(fileName, "only .html and .md files are fragments; ignored");
            continue;
        }
        std::string name = fragmentName(entry.path);
        auto earlier = fragmentSources.find(name);
        if (earlier != fragmentSources.end()) {
            diagnostics.error(fileName, "the fragment \"" + name + "\" is also " + codeFileName(earlier->second) +
                                            "; keep one of them");
            continue;
        }
        fragmentSources.emplace(name, entry.path);
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(fileName, file.error().message);
            continue;
        }
        if (extension == ".md") {
            MarkdownOptions options;
            options.profile = MarkdownProfile::Full;
            MarkdownResult md = renderMarkdown(file.value().contents(), options);
            if (!md.ok()) {
                diagnostics.error(fileName, *md.error);
                continue;
            }
            fragments.emplace(name, CompiledTemplate::fromMarkdown(entry.path, std::move(md.html)));
            continue;
        }
        // The layouts wrap whole pages, so each is a page itself.
        bool isLayout = name == kMarkdownLayout || name == kAuthLayout;
        ParsedTemplate parsed = parseTemplate(file.value(), isLayout ? TemplateKind::Page : TemplateKind::Fragment);
        report(parsed.errors, diagnostics);
        if (!parsed.ok()) continue;
        if (isLayout) {
            const auto& names = parsed.compiled.fieldNames();
            bool markdown = name == kMarkdownLayout;
            if (std::find(names.begin(), names.end(), "content") == names.end()) {
                diagnostics.error(fileName, markdown ? "the Markdown layout needs a {{content}} field, where each page goes"
                                                     : "the sign-in layout needs a {{content}} field, where the engine "
                                                       "puts its form");
                continue;
            }
            (markdown ? layout : authLayout) = std::move(parsed.compiled);
        } else {
            fragments.emplace(name, std::move(parsed.compiled));
        }
    }
    report(resolveIncludes(fragments), diagnostics);
    if (layout) {
        report(resolveIncludes(*layout, fragments), diagnostics);
        app.markdownLayout = std::move(layout);
    }
    if (authLayout) {
        report(resolveIncludes(*authLayout, fragments), diagnostics);
        app.authLayout = std::move(authLayout);
    }

    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(pages)) continue;
        if (lowerExtension(entry.path) != ".html") continue;
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(codeFileName(entry.path), file.error().message);
            continue;
        }
        ParsedTemplate parsed = parseTemplate(file.value(), TemplateKind::Page);
        report(parsed.errors, diagnostics);
        if (!parsed.ok()) continue;
        std::vector<TemplateProblem> problems = resolveIncludes(parsed.compiled, fragments);
        report(problems, diagnostics);
        if (problems.empty()) app.templates.add(entry.path, std::move(parsed.compiled));
    }
    app.templates.setFragments(std::move(fragments));
}

}  // namespace fourfold
