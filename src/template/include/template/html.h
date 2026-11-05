// Html: rendered, trusted markup (Arch §5 template; overview §4, Return
// values). A distinct type, so a plain string can never be taken for markup:
// strings put into fields are always escaped, and Html is inserted as is.
//
// Only three things make Html: rendering a template, rendering Markdown, and
// unsafeHtml (behind the unsafeHTML intrinsic). The first two build it
// through HtmlAccess, which is for the template and markdown components only.
//
// Html made from Markdown (an .md fragment, or markdown()) also has a
// Markdown form, so it can go into a Markdown template and be written out in
// either format (overview §4, Fields in Markdown).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fourfold {

// The Markdown form of rendered Markdown. Ready, or deferred: written the
// first time it's asked for (markdown()'s, which most requests never need).
// Not thread-safe; like the Html holding it, it's used by one thread at a time.
class MarkdownForm {
public:
    static std::shared_ptr<const MarkdownForm> ready(std::string text);
    // `make` returns nullopt if there's no Markdown form after all.
    static std::shared_ptr<const MarkdownForm> deferred(std::function<std::optional<std::string>()> make);
    // Markdown, whose form wasn't made: rendered for an HTML output only.
    static std::shared_ptr<const MarkdownForm> notMade();

    // Null if it wasn't made.
    const std::string* text() const;

private:
    mutable std::optional<std::string> text_;
    mutable std::function<std::optional<std::string>()> make_;
};

class Html {
public:
    Html() = default;   // empty markup

    const std::string& str() const { return markup_; }
    bool empty() const { return markup_.empty(); }
    // Contains a <script> element. Such markup can't be a fragment response:
    // its script wouldn't carry the page's CSP nonce (overview §8).
    bool hasScript() const { return hasScript_; }
    // Contains rendered Markdown, so a page holding it links markdown.css.
    bool hasMarkdown() const { return hasMarkdown_; }
    // Contains a form with a proof-of-work puzzle, so a page holding it
    // links the solver.
    bool hasChallenge() const { return hasChallenge_; }
    // It is Markdown (an .md fragment, or markdown()), so it can go in a
    // Markdown template. An HTML fragment, or HTML holding a Markdown value,
    // isn't.
    bool isMarkdown() const { return markdown_ != nullptr; }
    // Its Markdown form, for a Markdown output; null if it isn't Markdown, or
    // its form wasn't made (it was rendered for an HTML output).
    const std::string* markdown() const { return markdown_ ? markdown_->text() : nullptr; }

    // Appends `other`. The result is Markdown only if both are, and its
    // Markdown form is the two, as separate blocks.
    Html& operator+=(const Html& other);

private:
    friend struct HtmlAccess;
    Html(std::string markup, bool hasScript, bool hasMarkdown, std::shared_ptr<const MarkdownForm> markdown)
        : markup_(std::move(markup)), hasScript_(hasScript), hasMarkdown_(hasMarkdown), markdown_(std::move(markdown)) {}

    std::string markup_;
    bool hasScript_ = false;
    bool hasMarkdown_ = false;
    bool hasChallenge_ = false;
    std::shared_ptr<const MarkdownForm> markdown_;
};

// Markup from code, trusted as is: what unsafeHTML(text) returns. Its name is
// the warning. hasScript is set if the text contains "<script" in any case.
Html unsafeHtml(std::string markup);

// Markdown blocks joined into one Markdown text, with a blank line between
// each two.
void appendMarkdownBlock(std::string& out, const std::string& block);

// For the template and markdown components, which build Html from markup
// they escaped themselves. Nothing else should include this.
struct HtmlAccess {
    static Html make(std::string markup, bool hasScript, bool hasMarkdown,
                     std::shared_ptr<const MarkdownForm> markdown = nullptr) {
        return Html(std::move(markup), hasScript, hasMarkdown, std::move(markdown));
    }
    static Html withChallenge(Html html, bool hasChallenge) {
        html.hasChallenge_ = hasChallenge;
        return html;
    }
};

}  // namespace fourfold
