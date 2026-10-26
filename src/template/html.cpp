#include "template/html.h"

namespace fourfold {

Html& Html::operator+=(const Html& other) {
    markup_ += other.markup_;
    hasScript_ = hasScript_ || other.hasScript_;
    hasMarkdown_ = hasMarkdown_ || other.hasMarkdown_;
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
