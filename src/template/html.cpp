#include "template/html.h"

namespace fourfold {

std::shared_ptr<const MarkdownForm> MarkdownForm::ready(std::string text) {
    auto form = std::make_shared<MarkdownForm>();
    form->text_ = std::move(text);
    return form;
}

std::shared_ptr<const MarkdownForm> MarkdownForm::deferred(std::function<std::optional<std::string>()> make) {
    auto form = std::make_shared<MarkdownForm>();
    form->make_ = std::move(make);
    return form;
}

std::shared_ptr<const MarkdownForm> MarkdownForm::notMade() {
    return std::make_shared<MarkdownForm>();
}

const std::string* MarkdownForm::text() const {
    if (make_) {
        text_ = make_();
        make_ = nullptr;   // also lets go of what it captured
    }
    return text_ ? &*text_ : nullptr;
}

void appendMarkdownBlock(std::string& out, const std::string& block) {
    if (block.empty()) return;
    if (!out.empty()) {
        if (out.back() != '\n') out += '\n';
        out += '\n';
    }
    out += block;
    if (out.back() != '\n') out += '\n';
}

Html& Html::operator+=(const Html& other) {
    markup_ += other.markup_;
    hasScript_ = hasScript_ || other.hasScript_;
    hasMarkdown_ = hasMarkdown_ || other.hasMarkdown_;
    hasChallenge_ = hasChallenge_ || other.hasChallenge_;
    if (markdown_ && other.markdown_) {
        // Joined now, not deferred: frag("x", list) appends one item at a
        // time, and a chain of deferred joins would recurse once per item.
        const std::string* a = markdown_->text();
        const std::string* b = other.markdown_->text();
        if (a && b) {
            std::string joined = *a;
            appendMarkdownBlock(joined, *b);
            markdown_ = MarkdownForm::ready(std::move(joined));
        } else {
            markdown_ = MarkdownForm::notMade();
        }
    } else {
        markdown_ = nullptr;
    }
    return *this;
}

Html unsafeHtml(std::string markup) {
    static const char kScript[] = "<script";
    bool hasScript = false;
    for (size_t i = 0; !hasScript && i + 7 <= markup.size(); i++) {
        size_t j = 0;
        while (j < 7) {
            char c = markup[i + j];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != kScript[j]) break;
            j++;
        }
        hasScript = j == 7;
    }
    return HtmlAccess::make(std::move(markup), hasScript, false);
}

}  // namespace fourfold
