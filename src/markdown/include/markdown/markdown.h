// Fourfold Markdown: GitHub-flavored Markdown, made safe (overview §4,
// Markdown; Arch §5 markdown).
//
// - No raw HTML: tags in Markdown are shown as text.
// - Link and image URLs follow the template URL rule (http, https, mailto,
//   or relative); a link that breaks it is shown as plain text.
// - Two profiles. "comment" (the default for markdown() in code, since its
//   text is usually a visitor's): headings become paragraphs, images become
//   links, links get rel="nofollow ugc", and nothing gets an id. "full" (for
//   .md files in code/): headings get GitHub-style ids.
// - Task-list checkboxes are disabled.
// - The output is wrapped in <div class="markdown"> (or "markdown comment").
//
// md4c does the parsing (linear in its input, by design); the rendering is
// ours. Footnotes aren't supported yet (md4c has none; see m4-results.md).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "template/html.h"

namespace fourfold {

enum class MarkdownProfile { Comment, Full };

// "comment" or "full"; nullopt for anything else.
std::optional<MarkdownProfile> markdownProfileNamed(std::string_view name);

struct MarkdownOptions {
    MarkdownProfile profile = MarkdownProfile::Comment;
    // Full profile only: called for each link and image destination that
    // passed the URL rule. Returns a replacement destination, or nullopt to
    // keep it; may set `warning`. The compiler uses this to rewrite links
    // between .md files and to warn about broken ones.
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

MarkdownResult renderMarkdown(std::string_view text, const MarkdownOptions& options = {});

// The id GitHub gives a heading: lowercased, punctuation dropped, spaces as
// '-'. (Duplicates get "-1", "-2", ... from the renderer.)
std::string headingSlug(std::string_view text);

}  // namespace fourfold
