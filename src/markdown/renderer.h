// The markdown component's internals: md4c helpers, the HTML renderer, and
// the placeholders that stand for a Markdown template's fields while md4c
// parses it (markdown_template.cpp).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "markdown/markdown.h"
#include "md4c.h"

namespace fourfold::md {

void appendUtf8(std::string& out, unsigned cp);
// "&amp;", "&#106;", "&#x6A;" as text. md4c only reports well-formed ones.
void appendEntity(std::string& out, std::string_view entity);
// An attribute's text, with entities decoded.
std::string attributeText(const MD_ATTRIBUTE& a);
bool isLanguageChar(char c);
// md4c's flags for Fourfold Markdown: GFM, with no raw HTML.
constexpr unsigned kParserFlags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;

// --- Placeholders ---
//
// In a Markdown template, each {{...}} (and each reserved [[) is replaced,
// before md4c sees it, by a placeholder: U+FDD0, the index as hex digits
// U+FDE0–U+FDEF, then U+FDD1. These are Unicode noncharacters, set aside for
// a program's internal use, so they can't clash with text (a template that
// contains them is refused), and md4c treats them as ordinary text that
// isn't punctuation or space, so they parse as a word would.

std::string placeholder(size_t index);
// If a placeholder starts at s[at]: its index and byte length.
bool readPlaceholder(std::string_view s, size_t at, size_t& index, size_t& length);
bool hasPlaceholder(std::string_view s);
// Whether s contains any character in U+FDD0–U+FDEF.
bool hasNoncharacters(std::string_view s);

// Where a placeholder landed in the parse.
enum class Landing {
    Text,        // inline text
    Alt,         // an image's alt text, written as an attribute
    Title,       // a link's or image's title
    UrlStart,    // the start of a link's or image's destination
    UrlPart,     // later in a destination
    Block,       // alone in a top-level paragraph
    Code,        // in a code span or block: not a field
    Autolink,    // in an autolink
    Info,        // in a fenced code block's info string
    Brackets,    // in text inside [brackets] that aren't a link
    RefusedUrl,  // in a destination whose start breaks the URL rule
};

// The renderer's view of a template being parsed.
class TemplateScan {
public:
    virtual ~TemplateScan() = default;
    // The document md4c is parsing, with placeholders in it.
    virtual std::string_view document() const = 0;
    virtual void landed(size_t index, Landing where) = 0;
    // A placeholder's source text, for code: "{{name}}".
    virtual const std::string& original(size_t index) const = 0;
};

// Calls `each` for each placeholder in `s`, with its index, and returns s
// with each placeholder replaced by what `each` returns.
std::string replacePlaceholders(std::string_view s, const std::function<std::string(size_t)>& each);

// --- The HTML renderer ---

struct Renderer {
    const MarkdownOptions& options;
    bool full = false;
    TemplateScan* scan = nullptr;   // set when rendering a template
    MarkdownResult result;
    std::string out;

    // A heading being rendered: where it starts in `out`, and its text.
    bool inHeading = false;
    size_t headingStart = 0;
    std::string headingText;    // for its id: without placeholders
    std::string headingTitle;   // for the page's title: with them
    std::map<std::string, int> slugs;
    bool haveTitle = false;
    std::string titleWithFields;   // the first # heading's text, with placeholders

    // An image being rendered: its alt text is collected, not written.
    int imageDepth = 0;
    std::string imageAlt;
    std::optional<std::string> imageSrc;   // nullopt if it broke the URL rule
    std::string imageTitle;

    // Open links: true for each that was written (false if its URL was refused).
    std::vector<bool> links;

    // Templates: the open blocks, and the paragraph being rendered.
    int depth = 0;                   // open blocks, the document included
    bool paragraphTopLevel = false;
    size_t paragraphStart = 0;       // where its <p> is in `out`
    std::string paragraphText;       // its text, if it's only text so far
    bool paragraphPlain = true;      // nothing but text in it so far
    int bracketDepth = 0;            // unescaped literal [ in the current block's text

    Renderer(const MarkdownOptions& o) : options(o), full(o.profile == MarkdownProfile::Full) {}

    void text(std::string_view s);
    void normalText(std::string_view s);
    std::optional<std::string> url(const MD_ATTRIBUTE& a, bool isImage);
    std::string titleAttribute(const MD_ATTRIBUTE& a);
    void enterBlock(MD_BLOCKTYPE type, void* detail);
    void leaveBlock(MD_BLOCKTYPE type, void* detail);
    void enterSpan(MD_SPANTYPE type, void* detail);
    void leaveSpan(MD_SPANTYPE type);
    void onText(MD_TEXTTYPE type, std::string_view s);

    // Parses `text` into `out` (wrapped in its <div>); false if md4c failed.
    bool run(std::string_view text);

private:
    void landAll(std::string_view s, Landing where);
    bool escapedAt(const char* p) const;
};

}  // namespace fourfold::md
