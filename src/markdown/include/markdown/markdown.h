// FourFold Flavored Markdown: GitHub-flavored Markdown, made safe (overview
// §4, Markdown; Arch §5 markdown; docs/user/MARKDOWN.md).
//
// - No raw HTML: tags in Markdown are shown as text.
// - Link and image URLs follow the template URL rule (http, https, mailto,
//   or relative); a link that breaks it is shown as plain text.
// - Three levels. Limited Markdown (the "limited" profile, the default for
//   markdown() in code, since its text is usually a visitor's): headings
//   become paragraphs, images become links, links get rel="nofollow ugc",
//   and nothing gets an id. Full Markdown (the "full" profile): headings get
//   GitHub-style ids. Dynamic Markdown (.md files in code/): Full Markdown
//   with fields and includes, parsed into a template (parseMarkdownTemplate).
// - Task-list checkboxes are disabled.
// - The output is wrapped in <div class="markdown"> (or "markdown limited").
//
// md4c does the parsing (linear in its input, by design); the rendering is
// ours, and so is the Markdown writer (writeMarkdown), which writes rendered
// Markdown back out as Markdown for a Markdown output. Footnotes aren't
// supported yet (md4c has none; see m4-results.md).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "partition/code_tree.h"
#include "template/html.h"
#include "template/template.h"

namespace fourfold {

enum class MarkdownProfile { Limited, Full };

// "limited" or "full"; nullopt for anything else.
std::optional<MarkdownProfile> markdownProfileNamed(std::string_view name);

struct MarkdownOptions {
    MarkdownProfile profile = MarkdownProfile::Limited;
    // Full profile only: called for each link and image destination that
    // passed the URL rule (and has no fields). Returns a replacement
    // destination, or nullopt to keep it; may set `warning`. The compiler
    // uses this to rewrite links between .md files and to warn about broken
    // ones.
    std::function<std::optional<std::string>(const std::string& url, bool isImage, std::string& warning)>
        rewriteUrl;
    // The longest input accepted, in bytes; 0 for no limit.
    size_t maxBytes = 0;
};

struct MarkdownResult {
    Html html;
    std::string title;                   // the text of the first level-1 heading, if any
    std::vector<std::string> warnings;
    std::optional<std::string> error;    // set if it couldn't be rendered; html is then empty

    bool ok() const { return !error; }
};

// Rendered HTML. Its Markdown form isn't attached; markdownValue does that.
MarkdownResult renderMarkdown(std::string_view text, const MarkdownOptions& options = {});

// What markdown(text, profile) returns: the HTML, with a Markdown form that
// is written (by writeMarkdown) only if a Markdown output asks for it.
MarkdownResult markdownValue(std::string text, const MarkdownOptions& options = {});

// The text written back out as Markdown, under the profile's rules (overview
// §4, Fields in Markdown): what a Markdown client is sent for markdown(text).
// Limited Markdown's headings become paragraphs and its images links; links
// whose URLs break the URL rule become their text; raw HTML stays escaped
// text; reference links become inline ones. Parsing the result again gives
// the same structure, with no more than the profile allows.
std::string writeMarkdown(std::string_view text, MarkdownProfile profile);

// The id GitHub gives a heading: lowercased, punctuation dropped, spaces as
// '-'. (Duplicates get "-1", "-2", ... from the renderer.)
std::string headingSlug(std::string_view text);

// An error if `text` is over `maxBytes` (0: no limit) or too long to parse.
std::optional<std::string> checkMarkdownSize(std::string_view text, size_t maxBytes);

// --- Dynamic Markdown: .md files in code/ as templates ---

// Parses a .md page or fragment into a template with an HTML form (Full
// Markdown, rendered) and a Markdown form (its source, split at its fields),
// with fields and includes as in HTML templates (overview §4, Fields in
// Markdown). Every placement rule is checked here, so a template that breaks
// one never deploys. `options.rewriteUrl` rewrites literal link
// destinations (not ones with fields) in the HTML form; its warnings go into
// `warnings`.
ParsedTemplate parseMarkdownTemplate(const CodeFile& file, TemplateKind kind, const MarkdownOptions& options = {},
                                     std::vector<std::string>* warnings = nullptr);

}  // namespace fourfold
