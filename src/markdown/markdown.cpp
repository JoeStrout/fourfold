#include "markdown/markdown.h"

#include <map>

extern "C" {
#include "entity.h"   // md4c's entity table; its header has no extern "C"
}
#include "md4c.h"
#include "template/escape.h"

namespace fourfold {

namespace {

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

// "&amp;", "&#106;", "&#x6A;" as text. md4c only reports well-formed ones.
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

// An attribute's text, with entities decoded.
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

struct Renderer {
    const MarkdownOptions& options;
    bool full = false;
    MarkdownResult result;
    std::string out;

    // A heading being rendered: where it starts in `out`, and its text.
    bool inHeading = false;
    size_t headingStart = 0;
    std::string headingText;
    std::map<std::string, int> slugs;
    bool haveTitle = false;

    // An image being rendered: its alt text is collected, not written.
    int imageDepth = 0;
    std::string imageAlt;
    std::optional<std::string> imageSrc;   // nullopt if it broke the URL rule
    std::string imageTitle;

    // Open links: true for each that was written (false if its URL was refused).
    std::vector<bool> links;

    void text(std::string_view s) {
        if (imageDepth > 0) {
            imageAlt += s;
        } else {
            out += escapeHtml(s);
        }
        if (inHeading) headingText += s;
    }

    // The URL as written into an attribute, or nullopt if it's refused.
    std::optional<std::string> url(const MD_ATTRIBUTE& a, bool isImage) {
        std::string raw = attributeText(a);
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

    void enterBlock(MD_BLOCKTYPE type, void* detail) {
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
                } else {
                    out += "<p>";
                }
                break;
            case MD_BLOCK_CODE: {
                auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
                std::string lang = attributeText(d->lang);
                bool ok = !lang.empty();
                for (char c : lang) ok = ok && isLanguageChar(c);
                out += ok ? "<pre><code class=\"language-" + lang + "\">" : "<pre><code>";
                break;
            }
            case MD_BLOCK_HTML: break;   // not produced: raw HTML is off
            case MD_BLOCK_P: out += "<p>"; break;
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

    void leaveBlock(MD_BLOCKTYPE type, void* detail) {
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
                    result.title = headingText;
                    haveTitle = true;
                }
                inHeading = false;
                break;
            }
            case MD_BLOCK_CODE: out += "</code></pre>\n"; break;
            case MD_BLOCK_HTML: break;
            case MD_BLOCK_P: out += "</p>\n"; break;
            case MD_BLOCK_TABLE: out += "</table>\n"; break;
            case MD_BLOCK_THEAD: out += "</thead>\n"; break;
            case MD_BLOCK_TBODY: out += "</tbody>\n"; break;
            case MD_BLOCK_TR: out += "</tr>\n"; break;
            case MD_BLOCK_TH: out += "</th>"; break;
            case MD_BLOCK_TD: out += "</td>"; break;
        }
    }

    void enterSpan(MD_SPANTYPE type, void* detail) {
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
                std::optional<std::string> href = url(d->href, false);
                links.push_back(href.has_value());
                if (!href) break;
                out += "<a href=\"" + escapeHtml(*href) + "\"";
                if (d->title.size > 0) out += " title=\"" + escapeHtml(attributeText(d->title)) + "\"";
                if (!full) out += " rel=\"nofollow ugc\"";
                out += ">";
                break;
            }
            case MD_SPAN_IMG: {
                auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
                imageDepth = 1;
                imageAlt.clear();
                imageSrc = url(d->src, true);
                imageTitle = d->title.size > 0 ? attributeText(d->title) : "";
                break;
            }
            case MD_SPAN_LATEXMATH:
            case MD_SPAN_LATEXMATH_DISPLAY:
            case MD_SPAN_WIKILINK:
                break;   // not enabled
        }
    }

    void leaveSpan(MD_SPANTYPE type) {
        if (imageDepth > 0) {
            if (type != MD_SPAN_IMG || --imageDepth > 0) return;
            if (!imageSrc) {
                out += escapeHtml(imageAlt);
            } else if (full) {
                out += "<img src=\"" + escapeHtml(*imageSrc) + "\" alt=\"" + escapeHtml(imageAlt) + "\"";
                if (!imageTitle.empty()) out += " title=\"" + escapeHtml(imageTitle) + "\"";
                out += ">";
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

    void onText(MD_TEXTTYPE type, std::string_view s) {
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
                if (inHeading) headingText += ' ';
                break;
            case MD_TEXT_ENTITY: {
                std::string decoded;
                appendEntity(decoded, s);
                text(decoded);
                break;
            }
            default: text(s); break;   // normal text, code, and (never, with NOHTML) HTML
        }
    }
};

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

std::optional<MarkdownProfile> markdownProfileNamed(std::string_view name) {
    if (name == "comment") return MarkdownProfile::Comment;
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

MarkdownResult renderMarkdown(std::string_view text, const MarkdownOptions& options) {
    MarkdownResult failed;
    if (options.maxBytes > 0 && text.size() > options.maxBytes) {
        failed.error = "the text is " + std::to_string(text.size()) + " bytes, over the limit of " +
                       std::to_string(options.maxBytes);
        return failed;
    }
    if (text.size() > 0xFFFFFFF0u) {
        failed.error = "the text is too long";
        return failed;
    }

    Renderer r{options, options.profile == MarkdownProfile::Full, {}, {}, false, 0, {}, {}, false, 0, {}, {}, {}, {}};
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = enterBlockCallback;
    parser.leave_block = leaveBlockCallback;
    parser.enter_span = enterSpanCallback;
    parser.leave_span = leaveSpanCallback;
    parser.text = textCallback;

    r.out = r.full ? "<div class=\"markdown\">\n" : "<div class=\"markdown comment\">\n";
    if (md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &r) != 0) {
        failed.error = "the Markdown parser ran out of memory";
        return failed;
    }
    r.out += "</div>\n";
    r.result.html = HtmlAccess::make(std::move(r.out), false, true);
    return std::move(r.result);
}

}  // namespace fourfold
