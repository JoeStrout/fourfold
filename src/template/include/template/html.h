// Html: rendered, trusted markup (Arch §5 template; overview §4, Return
// values). A distinct type, so a plain string can never be taken for markup:
// strings put into fields are always escaped, and Html is inserted as is.
//
// Only three things make Html: rendering a template, rendering Markdown, and
// unsafeHtml (behind the unsafeHTML intrinsic). The first two build it
// through HtmlAccess, which is for the template and markdown components only.
#pragma once

#include <string>

namespace fourfold {

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

    Html& operator+=(const Html& other);

private:
    friend struct HtmlAccess;
    Html(std::string markup, bool hasScript, bool hasMarkdown)
        : markup_(std::move(markup)), hasScript_(hasScript), hasMarkdown_(hasMarkdown) {}

    std::string markup_;
    bool hasScript_ = false;
    bool hasMarkdown_ = false;
};

// Markup from code, trusted as is: what unsafeHTML(text) returns. Its name is
// the warning. hasScript is set if the text contains "<script" in any case.
Html unsafeHtml(std::string markup);

// For the template and markdown components, which build Html from markup
// they escaped themselves. Nothing else should include this.
struct HtmlAccess {
    static Html make(std::string markup, bool hasScript, bool hasMarkdown) {
        return Html(std::move(markup), hasScript, hasMarkdown);
    }
};

}  // namespace fourfold
