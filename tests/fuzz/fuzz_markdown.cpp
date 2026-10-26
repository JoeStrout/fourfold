// libFuzzer target: the Markdown renderer, both profiles. Crashes and
// sanitizer reports are what it looks for.
#include <cstddef>
#include <string_view>

#include "markdown/markdown.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    for (auto profile : {fourfold::MarkdownProfile::Comment, fourfold::MarkdownProfile::Full}) {
        fourfold::MarkdownOptions options;
        options.profile = profile;
        fourfold::renderMarkdown(text, options);
    }
    return 0;
}
