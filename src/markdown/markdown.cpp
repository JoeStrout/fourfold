#include "markdown/markdown.h"

#include <algorithm>

extern "C" {
#include "entity.h"   // md4c's entity table; its header has no extern "C"
}
#include "renderer.h"
#include "template/escape.h"

namespace fourfold {

namespace md {

void appendUtf8(std::string& out, unsigned cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void appendEntity(std::string& out, std::string_view entity) {
    if (entity.size() >= 4 && entity[1] == '#') {
        unsigned cp = 0;
        bool hex = entity[2] == 'x' || entity[2] == 'X';
        for (size_t i = hex ? 3 : 2; i + 1 < entity.size(); i++) {
            char c = entity[i];
            unsigned digit = (c >= '0' && c <= '9') ? unsigned(c - '0')
                             : (c >= 'a' && c <= 'f') ? unsigned(c - 'a' + 10)
                                                      : unsigned(c - 'A' + 10);
            cp = cp * (hex ? 16 : 10) + digit;
            if (cp > 0x10FFFF) cp = 0x110000;   // saturate: it's invalid either way
        }
        appendUtf8(out, cp);
        return;
    }
    const ENTITY* e = entity_lookup(entity.data(), entity.size());
    if (!e) {
        out += entity;
        return;
    }
    appendUtf8(out, e->codepoints[0]);
    if (e->codepoints[1]) appendUtf8(out, e->codepoints[1]);
}

std::string attributeText(const MD_ATTRIBUTE& a) {
    std::string out;
    // An empty attribute (an indented code block's language, say) may have
    // no substring arrays at all.
    if (a.size == 0 || !a.substr_offsets || !a.substr_types) return out;
    for (int i = 0; a.substr_offsets[i] < a.size; i++) {
        MD_OFFSET off = a.substr_offsets[i];
        MD_OFFSET end = a.substr_offsets[i + 1];
        std::string_view piece(a.text + off, end - off);
        switch (a.substr_types[i]) {
            case MD_TEXT_ENTITY: appendEntity(out, piece); break;
            case MD_TEXT_NULLCHAR: out += "\xEF\xBF\xBD"; break;
            default: out += piece;
        }
    }
    return out;
}

bool isLanguageChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '+' ||
           c == '-';
}

// --- Placeholders ---

namespace {

// U+FDD0 (start), U+FDD1 (end), and U+FDE0 + n (hex digit n), in UTF-8.
constexpr unsigned char kLead0 = 0xEF, kLead1 = 0xB7;
constexpr unsigned char kStart = 0x90, kEnd = 0x91, kDigit0 = 0xA0;

bool at3(std::string_view s, size_t at, unsigned char third) {
    return at + 2 < s.size() && static_cast<unsigned char>(s[at]) == kLead0 &&
           static_cast<unsigned char>(s[at + 1]) == kLead1 && static_cast<unsigned char>(s[at + 2]) == third;
}

}  // namespace

std::string placeholder(size_t index) {
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>(kDigit0 + (index & 15)));
        index >>= 4;
    } while (index > 0);
    std::string out;
    out += static_cast<char>(kLead0);
    out += static_cast<char>(kLead1);
    out += static_cast<char>(kStart);
    for (char d : digits) {
        out += static_cast<char>(kLead0);
        out += static_cast<char>(kLead1);
        out += d;
    }
    out += static_cast<char>(kLead0);
    out += static_cast<char>(kLead1);
    out += static_cast<char>(kEnd);
    return out;
}

bool readPlaceholder(std::string_view s, size_t at, size_t& index, size_t& length) {
    if (!at3(s, at, kStart)) return false;
    size_t i = at + 3;
    size_t value = 0;
    int digits = 0;
    while (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == kLead0 &&
           static_cast<unsigned char>(s[i + 1]) == kLead1) {
        auto c = static_cast<unsigned char>(s[i + 2]);
        if (c == kEnd) {
            if (digits == 0) return false;
            index = value;
            length = i + 3 - at;
            return true;
        }
        if (c < kDigit0 || c > kDigit0 + 15 || digits >= 12) return false;
        value = value * 16 + (c - kDigit0);
        digits++;
        i += 3;
    }
    return false;
}

bool hasPlaceholder(std::string_view s) {
    for (size_t i = 0; i + 2 < s.size(); i++) {
        if (at3(s, i, kStart)) return true;
    }
    return false;
}

bool hasNoncharacters(std::string_view s) {
    for (size_t i = 0; i + 2 < s.size(); i++) {
        if (static_cast<unsigned char>(s[i]) == kLead0 && static_cast<unsigned char>(s[i + 1]) == kLead1) {
            auto c = static_cast<unsigned char>(s[i + 2]);
            if (c >= 0x90 && c <= 0xAF) return true;
        }
    }
    return false;
}

std::string replacePlaceholders(std::string_view s, const std::function<std::string(size_t)>& each) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t index = 0, length = 0;
        if (readPlaceholder(s, i, index, length)) {
            out += each(index);
            i += length;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// --- The renderer ---

bool Renderer::escapedAt(const char* p) const {
    std::string_view doc = scan->document();
    if (p < doc.data() || p >= doc.data() + doc.size()) return false;
    size_t at = static_cast<size_t>(p - doc.data());
    size_t backslashes = 0;
    while (at > backslashes && doc[at - backslashes - 1] == '\\') backslashes++;
    return backslashes % 2 == 1;
}

void Renderer::landAll(std::string_view s, Landing where) {
    replacePlaceholders(s, [&](size_t index) {
        scan->landed(index, where);
        return std::string();
    });
}

void Renderer::text(std::string_view s) {
    if (imageDepth > 0) {
        imageAlt += s;
    } else {
        out += escapeHtml(s);
    }
    if (inHeading) {
        headingTitle += s;
        headingText += scan ? replacePlaceholders(s, [](size_t) { return std::string(); }) : std::string(s);
    }
}

// Normal text: in a template, each placeholder in it lands as text (unless
// it's in an image's alt text, which lands when the image is written).
void Renderer::normalText(std::string_view s) {
    if (scan && imageDepth == 0) {
        for (size_t i = 0; i < s.size();) {
            size_t index = 0, length = 0;
            if (readPlaceholder(s, i, index, length)) {
                scan->landed(index, bracketDepth > 0 ? Landing::Brackets : Landing::Text);
                i += length;
                continue;
            }
            // Literal brackets: a field inside them could become a link's
            // label when the Markdown output is parsed again.
            if ((s[i] == '[' || s[i] == ']') && !escapedAt(s.data() + i)) {
                bracketDepth += s[i] == '[' ? 1 : (bracketDepth > 0 ? -1 : 0);
            }
            i++;
        }
    }
    text(s);
}

std::optional<std::string> Renderer::url(const MD_ATTRIBUTE& a, bool isImage) {
    std::string raw = attributeText(a);
    if (scan && hasPlaceholder(raw)) {
        // A destination with fields: its start is checked now if it's
        // literal, or when it's filled if it's a field. It isn't rewritten.
        std::string href;
        bool first = true;
        size_t pieceStart = 0;
        for (size_t i = 0; i <= raw.size();) {
            size_t index = 0, length = 0;
            bool ph = i < raw.size() && readPlaceholder(raw, i, index, length);
            if (!ph && i < raw.size()) {
                i++;
                continue;
            }
            std::string_view piece(raw.data() + pieceStart, i - pieceStart);
            if (!piece.empty()) {
                if (first) {
                    std::optional<std::string> start = filterUrl(piece);
                    if (!start) {
                        landAll(raw, Landing::RefusedUrl);
                        return std::nullopt;
                    }
                    href += *start;
                } else {
                    href += normalizeUrlPiece(piece);
                }
                first = false;
            }
            if (!ph) break;
            scan->landed(index, first ? Landing::UrlStart : Landing::UrlPart);
            href += raw.substr(i, length);
            first = false;
            i += length;
            pieceStart = i;
        }
        return href;
    }
    std::optional<std::string> filtered = filterUrl(raw);
    if (!filtered) return std::nullopt;
    if (full && options.rewriteUrl) {
        std::string warning;
        std::optional<std::string> rewritten = options.rewriteUrl(*filtered, isImage, warning);
        if (!warning.empty()) result.warnings.push_back(warning);
        if (rewritten) filtered = rewritten;
    }
    return filtered;
}

std::string Renderer::titleAttribute(const MD_ATTRIBUTE& a) {
    std::string title = attributeText(a);
    if (scan) landAll(title, Landing::Title);
    return title;
}

void Renderer::enterBlock(MD_BLOCKTYPE type, void* detail) {
    bool topLevel = depth == 1;
    depth++;
    bracketDepth = 0;
    switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: out += "<blockquote>\n"; break;
        case MD_BLOCK_UL: out += "<ul>\n"; break;
        case MD_BLOCK_OL: {
            auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            if (d->start == 1) {
                out += "<ol>\n";
            } else {
                out += "<ol start=\"" + std::to_string(d->start) + "\">\n";
            }
            break;
        }
        case MD_BLOCK_LI: {
            auto* d = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            if (d->is_task) {
                out += "<li class=\"task-list-item\"><input type=\"checkbox\" disabled";
                if (d->task_mark == 'x' || d->task_mark == 'X') out += " checked";
                out += "> ";
            } else {
                out += "<li>";
            }
            break;
        }
        case MD_BLOCK_HR: out += "<hr>\n"; break;
        case MD_BLOCK_H:
            if (full) {
                inHeading = true;
                headingStart = out.size();
                headingText.clear();
                headingTitle.clear();
            } else {
                out += "<p>";
            }
            break;
        case MD_BLOCK_CODE: {
            auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            if (scan) landAll(attributeText(d->info), Landing::Info);
            std::string lang = attributeText(d->lang);
            bool ok = !lang.empty();
            for (char c : lang) ok = ok && isLanguageChar(c);
            out += ok ? "<pre><code class=\"language-" + lang + "\">" : "<pre><code>";
            break;
        }
        case MD_BLOCK_HTML: break;   // not produced: raw HTML is off
        case MD_BLOCK_P:
            paragraphTopLevel = topLevel;
            paragraphStart = out.size();
            out += "<p>";
            break;
        case MD_BLOCK_TABLE: out += "<table>\n"; break;
        case MD_BLOCK_THEAD: out += "<thead>\n"; break;
        case MD_BLOCK_TBODY: out += "<tbody>\n"; break;
        case MD_BLOCK_TR: out += "<tr>"; break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            auto* d = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
            out += type == MD_BLOCK_TH ? "<th" : "<td";
            switch (d->align) {
                case MD_ALIGN_LEFT: out += " align=\"left\""; break;
                case MD_ALIGN_CENTER: out += " align=\"center\""; break;
                case MD_ALIGN_RIGHT: out += " align=\"right\""; break;
                case MD_ALIGN_DEFAULT: break;
            }
            out += ">";
            break;
        }
    }
}

void Renderer::leaveBlock(MD_BLOCKTYPE type, void* detail) {
    depth--;
    bracketDepth = 0;
    switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: out += "</blockquote>\n"; break;
        case MD_BLOCK_UL: out += "</ul>\n"; break;
        case MD_BLOCK_OL: out += "</ol>\n"; break;
        case MD_BLOCK_LI: out += "</li>\n"; break;
        case MD_BLOCK_HR: break;
        case MD_BLOCK_H: {
            if (!full) {
                out += "</p>\n";
                break;
            }
            unsigned level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
            std::string h = "h" + std::to_string(level);
            std::string slug = headingSlug(headingText);
            int& seen = slugs[slug];
            if (seen > 0) slug += "-" + std::to_string(seen);
            seen++;
            out.insert(headingStart, "<" + h + " id=\"" + escapeHtml(slug) + "\">");
            out += "</" + h + ">\n";
            if (level == 1 && !haveTitle) {
                result.title = headingTitle;
                haveTitle = true;
            }
            inHeading = false;
            break;
        }
        case MD_BLOCK_CODE: out += "</code></pre>\n"; break;
        case MD_BLOCK_HTML: break;
        case MD_BLOCK_P: {
            // In a template, a top-level paragraph that's one field alone is
            // a block field: the field stands for the paragraph.
            size_t index = 0, length = 0;
            size_t inner = paragraphStart + 3;
            if (scan && paragraphTopLevel && readPlaceholder(out, inner, index, length) &&
                inner + length == out.size()) {
                scan->landed(index, Landing::Block);
                out.erase(paragraphStart, 3);
                break;
            }
            out += "</p>\n";
            break;
        }
        case MD_BLOCK_TABLE: out += "</table>\n"; break;
        case MD_BLOCK_THEAD: out += "</thead>\n"; break;
        case MD_BLOCK_TBODY: out += "</tbody>\n"; break;
        case MD_BLOCK_TR: out += "</tr>\n"; break;
        case MD_BLOCK_TH: out += "</th>"; break;
        case MD_BLOCK_TD: out += "</td>"; break;
    }
}

void Renderer::enterSpan(MD_SPANTYPE type, void* detail) {
    if (imageDepth > 0) {
        if (type == MD_SPAN_IMG) imageDepth++;
        return;   // inside an image, only text counts (as alt text)
    }
    switch (type) {
        case MD_SPAN_EM: out += "<em>"; break;
        case MD_SPAN_STRONG: out += "<strong>"; break;
        case MD_SPAN_CODE: out += "<code>"; break;
        case MD_SPAN_DEL: out += "<del>"; break;
        case MD_SPAN_U: out += "<u>"; break;
        case MD_SPAN_A: {
            auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
            if (scan && d->is_autolink && hasPlaceholder(attributeText(d->href))) {
                landAll(attributeText(d->href), Landing::Autolink);
                links.push_back(false);
                break;
            }
            std::optional<std::string> href = url(d->href, false);
            links.push_back(href.has_value());
            if (!href) break;
            out += "<a href=\"" + escapeHtml(*href) + "\"";
            if (d->title.size > 0) out += " title=\"" + escapeHtml(titleAttribute(d->title)) + "\"";
            if (!full) out += " rel=\"nofollow ugc\"";
            out += ">";
            break;
        }
        case MD_SPAN_IMG: {
            auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
            imageDepth = 1;
            imageAlt.clear();
            imageSrc = url(d->src, true);
            imageTitle = d->title.size > 0 ? titleAttribute(d->title) : "";
            break;
        }
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
        case MD_SPAN_WIKILINK:
            break;   // not enabled
    }
}

void Renderer::leaveSpan(MD_SPANTYPE type) {
    if (imageDepth > 0) {
        if (type != MD_SPAN_IMG || --imageDepth > 0) return;
        if (!imageSrc) {
            if (scan) landAll(imageAlt, Landing::Text);
            out += escapeHtml(imageAlt);
        } else if (full) {
            if (scan) landAll(imageAlt, Landing::Alt);
            out += "<img src=\"" + escapeHtml(*imageSrc) + "\" alt=\"" + escapeHtml(imageAlt) + "\"";
            if (!imageTitle.empty()) out += " title=\"" + escapeHtml(imageTitle) + "\"";
            out += ">";
        } else if (std::find(links.begin(), links.end(), true) != links.end()) {
            // Inside a link, a link to the image would nest: its text alone.
            out += escapeHtml(imageAlt.empty() ? *imageSrc : imageAlt);
        } else {
            // A visitor's image would be a tracking pixel or a hotlink:
            // show a link to it instead.
            out += "<a href=\"" + escapeHtml(*imageSrc) + "\" rel=\"nofollow ugc\">" +
                   escapeHtml(imageAlt.empty() ? *imageSrc : imageAlt) + "</a>";
        }
        return;
    }
    switch (type) {
        case MD_SPAN_EM: out += "</em>"; break;
        case MD_SPAN_STRONG: out += "</strong>"; break;
        case MD_SPAN_CODE: out += "</code>"; break;
        case MD_SPAN_DEL: out += "</del>"; break;
        case MD_SPAN_U: out += "</u>"; break;
        case MD_SPAN_A:
            if (!links.empty()) {
                if (links.back()) out += "</a>";
                links.pop_back();
            }
            break;
        default: break;
    }
}

void Renderer::onText(MD_TEXTTYPE type, std::string_view s) {
    switch (type) {
        case MD_TEXT_NULLCHAR: text("\xEF\xBF\xBD"); break;
        case MD_TEXT_BR:
            if (imageDepth > 0) {
                imageAlt += ' ';
            } else {
                out += "<br>\n";
            }
            break;
        case MD_TEXT_SOFTBR:
            if (imageDepth > 0) {
                imageAlt += ' ';
            } else {
                out += "\n";
            }
            if (inHeading) {
                headingText += ' ';
                headingTitle += ' ';
            }
            break;
        case MD_TEXT_ENTITY: {
            std::string decoded;
            appendEntity(decoded, s);
            text(decoded);
            break;
        }
        case MD_TEXT_CODE:
            if (scan) {
                // A {{...}} in code is text, as written.
                text(replacePlaceholders(s, [this](size_t index) {
                    scan->landed(index, Landing::Code);
                    return scan->original(index);
                }));
            } else {
                text(s);
            }
            break;
        case MD_TEXT_NORMAL: normalText(s); break;
        default: text(s); break;   // (never, with NOHTML) HTML
    }
}

namespace {

int enterBlockCallback(MD_BLOCKTYPE type, void* detail, void* user) {
    static_cast<Renderer*>(user)->enterBlock(type, detail);
    return 0;
}
int leaveBlockCallback(MD_BLOCKTYPE type, void* detail, void* user) {
    static_cast<Renderer*>(user)->leaveBlock(type, detail);
    return 0;
}
int enterSpanCallback(MD_SPANTYPE type, void* detail, void* user) {
    static_cast<Renderer*>(user)->enterSpan(type, detail);
    return 0;
}
int leaveSpanCallback(MD_SPANTYPE type, void*, void* user) {
    static_cast<Renderer*>(user)->leaveSpan(type);
    return 0;
}
int textCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
    static_cast<Renderer*>(user)->onText(type, std::string_view(text, size));
    return 0;
}

}  // namespace

bool Renderer::run(std::string_view text) {
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = kParserFlags;
    parser.enter_block = enterBlockCallback;
    parser.leave_block = leaveBlockCallback;
    parser.enter_span = enterSpanCallback;
    parser.leave_span = leaveSpanCallback;
    parser.text = textCallback;

    out = full ? "<div class=\"markdown\">\n" : "<div class=\"markdown limited\">\n";
    if (md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, this) != 0) return false;
    out += "</div>\n";
    return true;
}

}  // namespace md

std::optional<MarkdownProfile> markdownProfileNamed(std::string_view name) {
    if (name == "limited") return MarkdownProfile::Limited;
    if (name == "full") return MarkdownProfile::Full;
    return std::nullopt;
}

std::string headingSlug(std::string_view text) {
    std::string slug;
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') {
            slug += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80) {
            slug += ch;
        } else if (c == ' ') {
            slug += '-';
        }
    }
    return slug;
}

std::optional<std::string> checkMarkdownSize(std::string_view text, size_t maxBytes) {
    if (maxBytes > 0 && text.size() > maxBytes) {
        return "the text is " + std::to_string(text.size()) + " bytes, over the limit of " + std::to_string(maxBytes);
    }
    if (text.size() > 0xFFFFFFF0u) return std::string("the text is too long");
    return std::nullopt;
}

MarkdownResult renderMarkdown(std::string_view text, const MarkdownOptions& options) {
    MarkdownResult failed;
    if (auto error = checkMarkdownSize(text, options.maxBytes)) {
        failed.error = *error;
        return failed;
    }
    md::Renderer r(options);
    if (!r.run(text)) {
        failed.error = "the Markdown parser ran out of memory";
        return failed;
    }
    r.result.html = HtmlAccess::make(std::move(r.out), false, true);
    return std::move(r.result);
}

}  // namespace fourfold
