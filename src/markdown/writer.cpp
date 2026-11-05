// The Markdown writer: rendered Markdown written back out as Markdown, for a
// Markdown output (overview §4, Fields in Markdown). It's what a Markdown
// client gets for markdown(text): the text as md4c parsed it, under the
// profile's rules, written so that parsing it again gives the same
// structure and nothing more.
//
// - Text is escaped wherever a character could start or end something:
//   always \ ` * _ [ ] < > & | ~ # @, and others where they'd matter (a
//   '-', '+' or '=' first on a line; "1." first on a line; "www."; "://").
//   Spaces first on a line are written as &#32;.
// - Links and images are written inline, never as references, with their
//   URLs checked by the URL rule (a refused one leaves its text).
// - Limited Markdown: headings become paragraphs and images links, as the
//   HTML renderer does.
// - Code blocks are always fenced, with a fence longer than any run of
//   backticks inside.
// - No raw HTML: md4c parses with it off, so tags arrive as text, and '<' is
//   always escaped.

#include <algorithm>

#include "markdown/markdown.h"
#include "renderer.h"
#include "template/escape.h"

namespace fourfold {

namespace {

bool isAsciiPunctuation(unsigned char c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

size_t longestRun(const std::string& s, char c) {
    size_t longest = 0, run = 0;
    for (char ch : s) {
        run = ch == c ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return longest;
}

struct Writer {
    bool full = false;
    std::string out;

    // A container's line prefix: "> " for a quote; for a list item, its
    // marker on its first line and spaces after.
    struct Prefix {
        bool quote = false;
        std::string marker;   // "- ", "1. "
        bool markerWritten = false;
    };
    std::vector<Prefix> prefixes;

    // Each open block that holds blocks: whether a block has started in it
    // yet, and whether its blocks are separated by a line end only (a tight
    // list, or an item in one) rather than a blank line.
    struct Level {
        bool first = true;
        bool tight = false;
    };
    std::vector<Level> levels;

    struct List {
        bool ordered = false;
        char mark = '-';       // '-', '*', '+'; or '.', ')' for an ordered list
        unsigned next = 1;
        bool tight = false;
    };
    std::vector<List> lists;

    bool lineOpen = false;       // the current line's prefix is written
    std::string pendingTask;     // "[x] ", written with a task item's first line
    size_t contentStart = 0;     // where the current line's content starts in `out`

    // Inline state.
    bool inHeading = false;      // an ATX heading: line breaks become spaces
    int imageDepth = 0;
    bool imageAsLink = false;    // Limited: the image is written as a link
    bool imageAsText = false;    // its URL was refused, or it's in a link: alt text only
    size_t imageTextStart = 0;
    std::string imageSrc, imageTitle;
    struct Link {
        bool written = false;
        std::string dest, title;
    };
    std::vector<Link> links;
    std::vector<std::string> emphasis;   // the delimiter each open span used
    bool inCodeSpan = false;
    std::string code;                    // a code span's or block's text, collected
    std::string codeInfo;
    std::vector<MD_ALIGN> aligns;
    bool inHead = false;

    // --- Lines and blocks ---

    void ensureLine() {
        if (lineOpen) return;
        for (Prefix& p : prefixes) {
            if (p.quote) {
                out += "> ";
            } else if (!p.markerWritten) {
                out += p.marker;
                p.markerWritten = true;
            } else {
                out.append(p.marker.size(), ' ');
            }
        }
        lineOpen = true;
        contentStart = out.size();
        if (!pendingTask.empty()) {
            out += pendingTask;
            pendingTask.clear();
            contentStart = std::string::npos;   // a task's text starts after its box, not a line start
        }
        if (!levels.empty()) levels.back().first = false;   // text in a tight item is its first block
    }

    void newline() {
        if (!lineOpen) return;
        out += '\n';
        lineOpen = false;
    }

    void blankLine() {
        newline();
        std::string prefix;
        for (const Prefix& p : prefixes) {
            if (p.quote) {
                prefix += "> ";
            } else {
                prefix.append(p.marker.size(), ' ');
            }
        }
        while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
        out += prefix + "\n";
    }

    // Separates a new block from the block before it in the same container.
    void beginBlock() {
        if (levels.empty()) return;
        Level& level = levels.back();
        if (!level.first) {
            if (level.tight) {
                newline();
            } else {
                blankLine();
            }
        }
        level.first = false;
    }

    bool atContentStart() const { return lineOpen && out.size() == contentStart; }

    // --- Text ---

    bool lineStartDigits() const {
        if (!lineOpen || contentStart >= out.size()) return false;
        for (size_t i = contentStart; i < out.size(); i++) {
            if (out[i] < '0' || out[i] > '9') return false;
        }
        return true;
    }

    bool endsWithWww() const {
        if (out.size() < 3) return false;
        for (size_t i = out.size() - 3; i < out.size(); i++) {
            if (out[i] != 'w' && out[i] != 'W') return false;
        }
        return true;
    }

    void writeText(std::string_view s) {
        ensureLine();
        for (char ch : s) {
            auto c = static_cast<unsigned char>(ch);
            bool escape = false;
            switch (c) {
                case '\\': case '`': case '*': case '_': case '[': case ']': case '<': case '>':
                case '&': case '|': case '~': case '#': case '@':
                    escape = true;
                    break;
                case '/': escape = !out.empty() && out.back() == ':'; break;   // "://" starts an autolink
                case '.': escape = lineStartDigits() || endsWithWww(); break;
                case ')': escape = lineStartDigits(); break;
                case '-': case '+': case '=': escape = atContentStart(); break;
                case ' ':
                    if (atContentStart()) {
                        out += "&#32;";
                        continue;
                    }
                    break;
                case '\t':
                    if (atContentStart()) {
                        out += "&#9;";
                        continue;
                    }
                    break;
                default: break;
            }
            if (escape) out += '\\';
            out += ch;
        }
    }

    // A '!' just written, before a link's '[': escaped, or it would make an
    // image.
    void escapeBang() {
        if (out.empty() || out.back() != '!') return;
        size_t n = 0;
        while (out.size() >= n + 2 && out[out.size() - n - 2] == '\\') n++;
        if (n % 2 == 0) out.insert(out.size() - 1, "\\");
    }

    static std::string escapeTitle(const std::string& title) {
        std::string out;
        for (char ch : title) {
            auto c = static_cast<unsigned char>(ch);
            if (ch == '\n' || ch == '\r' || ch == '\t') {
                out += ' ';
                continue;
            }
            if (isAsciiPunctuation(c)) out += '\\';
            out += ch;
        }
        return out;
    }

    // A URL that passed the rule (so it has no spaces, <, > or control
    // characters), as a link destination: in <...> if it has parentheses,
    // which would otherwise have to balance.
    static std::string destination(const std::string& url, const std::string& title) {
        bool parens = url.find_first_of("()") != std::string::npos;
        std::string s = parens ? "(<" + url + ">" : "(" + url;
        if (!title.empty()) s += " \"" + escapeTitle(title) + "\"";
        return s + ")";
    }

    // (A '|' in code in a table cell stays as it is: md4c doesn't split a
    // cell inside a code span.)
    void writeCodeSpan() {
        const std::string& c = code;
        std::string fence(longestRun(c, '`') + 1, '`');
        bool allSpaces = std::all_of(c.begin(), c.end(), [](char ch) { return ch == ' '; });
        bool pad = !c.empty() && (c.front() == '`' || c.back() == '`' ||
                                  (!allSpaces && c.front() == ' ' && c.back() == ' '));
        ensureLine();
        out += fence;
        if (pad) out += ' ';
        out += c;
        if (pad) out += ' ';
        out += fence;
    }

    void writeCodeBlock() {
        size_t backticks = std::max<size_t>(3, longestRun(code, '`') + 1);
        std::string fence(backticks, '`');
        ensureLine();
        out += fence + codeInfo;
        newline();
        size_t start = 0;
        while (start < code.size()) {
            size_t end = code.find('\n', start);
            if (end == std::string::npos) end = code.size();
            ensureLine();
            out.append(code, start, end - start);
            newline();
            start = end + 1;
        }
        ensureLine();
        out += fence;
    }

    // --- md4c's events ---

    void enterBlock(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
            case MD_BLOCK_DOC:
                levels.push_back({true, false});
                break;
            case MD_BLOCK_QUOTE:
                beginBlock();
                prefixes.push_back({true, "", true});
                levels.push_back({true, false});
                break;
            case MD_BLOCK_UL: {
                auto* d = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
                beginBlock();
                lists.push_back({false, d->mark ? d->mark : '-', 1, d->is_tight != 0});
                levels.push_back({true, d->is_tight != 0});
                break;
            }
            case MD_BLOCK_OL: {
                auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
                beginBlock();
                lists.push_back({true, d->mark_delimiter ? d->mark_delimiter : '.', d->start, d->is_tight != 0});
                levels.push_back({true, d->is_tight != 0});
                break;
            }
            case MD_BLOCK_LI: {
                auto* d = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                beginBlock();
                List& list = lists.back();
                std::string marker = list.ordered ? std::to_string(list.next++) + list.mark + " "
                                                  : std::string(1, list.mark) + " ";
                prefixes.push_back({false, marker, false});
                levels.push_back({true, list.tight});
                if (d->is_task) pendingTask = (d->task_mark == 'x' || d->task_mark == 'X') ? "[x] " : "[ ] ";
                break;
            }
            case MD_BLOCK_HR:
                beginBlock();
                ensureLine();
                out += "***";
                break;
            case MD_BLOCK_H: {
                beginBlock();
                if (full) {
                    unsigned level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
                    ensureLine();
                    out += std::string(level, '#') + " ";
                    contentStart = std::string::npos;
                    inHeading = true;
                }
                break;
            }
            case MD_BLOCK_CODE: {
                auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
                beginBlock();
                std::string lang = md::attributeText(d->lang);
                bool ok = !lang.empty();
                for (char c : lang) ok = ok && md::isLanguageChar(c);
                codeInfo = ok ? lang : "";
                code.clear();
                break;
            }
            case MD_BLOCK_HTML: break;   // not produced: raw HTML is off
            case MD_BLOCK_P: beginBlock(); break;
            case MD_BLOCK_TABLE:
                beginBlock();
                aligns.clear();
                break;
            case MD_BLOCK_THEAD: inHead = true; break;
            case MD_BLOCK_TBODY: break;
            case MD_BLOCK_TR:
                ensureLine();
                out += "|";
                contentStart = std::string::npos;
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                if (type == MD_BLOCK_TH && inHead) aligns.push_back(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align);
                out += " ";
                break;
        }
    }

    void leaveBlock(MD_BLOCKTYPE type, void*) {
        switch (type) {
            case MD_BLOCK_DOC:
                newline();
                levels.pop_back();
                break;
            case MD_BLOCK_QUOTE:
                if (levels.back().first) {   // an empty quote
                    ensureLine();
                    out.pop_back();   // "> " → ">"
                }
                prefixes.pop_back();
                levels.pop_back();
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                lists.pop_back();
                levels.pop_back();
                break;
            case MD_BLOCK_LI:
                if (!prefixes.back().markerWritten) ensureLine();   // an empty item
                prefixes.pop_back();
                levels.pop_back();
                break;
            case MD_BLOCK_HR: break;
            case MD_BLOCK_H: inHeading = false; break;
            case MD_BLOCK_CODE: writeCodeBlock(); break;
            case MD_BLOCK_HTML: break;
            case MD_BLOCK_P: break;
            case MD_BLOCK_TABLE: break;
            case MD_BLOCK_THEAD: {
                inHead = false;
                newline();
                ensureLine();
                out += "|";
                for (MD_ALIGN a : aligns) {
                    switch (a) {
                        case MD_ALIGN_LEFT: out += " :-- |"; break;
                        case MD_ALIGN_CENTER: out += " :-: |"; break;
                        case MD_ALIGN_RIGHT: out += " --: |"; break;
                        case MD_ALIGN_DEFAULT: out += " --- |"; break;
                    }
                }
                newline();
                break;
            }
            case MD_BLOCK_TBODY: break;
            case MD_BLOCK_TR: newline(); break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                out += " |";
                break;
        }
    }

    bool insideWrittenLink() const {
        for (const Link& l : links) {
            if (l.written) return true;
        }
        return false;
    }

    void enterSpan(MD_SPANTYPE type, void* detail) {
        if (imageDepth > 0) {
            if (type == MD_SPAN_IMG) imageDepth++;
            return;   // inside an image, only its text counts (as alt text)
        }
        switch (type) {
            case MD_SPAN_EM:
            case MD_SPAN_STRONG:
            case MD_SPAN_DEL: {
                ensureLine();
                // Right after another delimiter, '*' would run into it.
                bool afterStar = !out.empty() && out.back() == '*' && out.size() > contentStart;
                std::string d = type == MD_SPAN_DEL ? "~~"
                                : type == MD_SPAN_STRONG ? (afterStar ? "__" : "**")
                                                         : (afterStar ? "_" : "*");
                emphasis.push_back(d);
                out += d;
                break;
            }
            case MD_SPAN_CODE:
                inCodeSpan = true;
                code.clear();
                break;
            case MD_SPAN_A: {
                auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
                std::optional<std::string> href = filterUrl(md::attributeText(d->href));
                Link link;
                link.written = href.has_value();
                if (href) {
                    link.dest = *href;
                    link.title = d->title.size > 0 ? md::attributeText(d->title) : "";
                    ensureLine();
                    escapeBang();
                    out += "[";
                }
                links.push_back(std::move(link));
                break;
            }
            case MD_SPAN_IMG: {
                auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
                imageDepth = 1;
                std::optional<std::string> src = filterUrl(md::attributeText(d->src));
                imageAsText = !src || (!full && insideWrittenLink());
                imageAsLink = !full && !imageAsText;
                imageSrc = src ? *src : "";
                imageTitle = d->title.size > 0 ? md::attributeText(d->title) : "";
                ensureLine();
                if (!imageAsText) {
                    if (imageAsLink) escapeBang();
                    out += imageAsLink ? "[" : "![";
                }
                imageTextStart = out.size();
                break;
            }
            default: break;   // not enabled
        }
    }

    void leaveSpan(MD_SPANTYPE type) {
        if (imageDepth > 0) {
            if (type != MD_SPAN_IMG || --imageDepth > 0) return;
            if (imageAsText) return;
            if (imageAsLink) {
                if (out.size() == imageTextStart) writeText(imageSrc);   // no alt text: the URL is the text
                out += "]" + destination(imageSrc, "");
            } else {
                out += "]" + destination(imageSrc, imageTitle);
            }
            return;
        }
        switch (type) {
            case MD_SPAN_EM:
            case MD_SPAN_STRONG:
            case MD_SPAN_DEL:
                if (!emphasis.empty()) {
                    out += emphasis.back();
                    emphasis.pop_back();
                }
                break;
            case MD_SPAN_CODE:
                inCodeSpan = false;
                writeCodeSpan();
                break;
            case MD_SPAN_A:
                if (!links.empty()) {
                    if (links.back().written) out += "]" + destination(links.back().dest, links.back().title);
                    links.pop_back();
                }
                break;
            default: break;
        }
    }

    void onText(MD_TEXTTYPE type, std::string_view s) {
        if (inCodeSpan) {
            code += type == MD_TEXT_SOFTBR || type == MD_TEXT_BR ? std::string_view(" ") : s;
            return;
        }
        switch (type) {
            case MD_TEXT_CODE: code += s; break;   // a code block's text
            case MD_TEXT_NULLCHAR: writeText("\xEF\xBF\xBD"); break;
            case MD_TEXT_BR:
                if (imageDepth > 0 || inHeading) {
                    writeText(" ");
                } else {
                    ensureLine();
                    out += "\\";
                    newline();
                }
                break;
            case MD_TEXT_SOFTBR:
                if (imageDepth > 0 || inHeading) {
                    writeText(" ");
                } else {
                    newline();
                }
                break;
            case MD_TEXT_ENTITY:
                // As written: an entity reference is text, whatever it stands for.
                ensureLine();
                out += s;
                break;
            default: writeText(s); break;   // normal text, and (never, with NOHTML) HTML
        }
    }
};

int enterBlockCallback(MD_BLOCKTYPE type, void* detail, void* user) {
    static_cast<Writer*>(user)->enterBlock(type, detail);
    return 0;
}
int leaveBlockCallback(MD_BLOCKTYPE type, void* detail, void* user) {
    static_cast<Writer*>(user)->leaveBlock(type, detail);
    return 0;
}
int enterSpanCallback(MD_SPANTYPE type, void* detail, void* user) {
    static_cast<Writer*>(user)->enterSpan(type, detail);
    return 0;
}
int leaveSpanCallback(MD_SPANTYPE type, void*, void* user) {
    static_cast<Writer*>(user)->leaveSpan(type);
    return 0;
}
int textCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
    static_cast<Writer*>(user)->onText(type, std::string_view(text, size));
    return 0;
}

}  // namespace

std::string writeMarkdown(std::string_view text, MarkdownProfile profile) {
    if (text.size() > 0xFFFFFFF0u) return "";
    Writer w;
    w.full = profile == MarkdownProfile::Full;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = md::kParserFlags;
    parser.enter_block = enterBlockCallback;
    parser.leave_block = leaveBlockCallback;
    parser.enter_span = enterSpanCallback;
    parser.leave_span = leaveSpanCallback;
    parser.text = textCallback;
    if (md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &w) != 0) return "";
    return std::move(w.out);
}

MarkdownResult markdownValue(std::string text, const MarkdownOptions& options) {
    MarkdownResult r = renderMarkdown(text, options);
    if (!r.ok()) return r;
    MarkdownProfile profile = options.profile;
    auto form = MarkdownForm::deferred([text = std::move(text), profile]() -> std::optional<std::string> {
        return writeMarkdown(text, profile);
    });
    r.html = HtmlAccess::make(r.html.str(), r.html.hasScript(), r.html.hasMarkdown(), std::move(form));
    return r;
}

}  // namespace fourfold
