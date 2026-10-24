#include "template/template.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace fourfold {

namespace {

// Elements whose content isn't ordinary text: the HTML tokenizer treats it
// as raw text or RCDATA, so escaping rules differ (or don't apply at all).
const char* const kRawTextElements[] = {"script", "style",    "textarea", "title",    "xmp",
                                        "iframe", "noembed", "noframes", "noscript", "plaintext"};

bool isRawTextElement(const std::string& name) {
    for (const char* e : kRawTextElements) {
        if (name == e) return true;
    }
    return false;
}

bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isIdentifierStart(char c) {
    return isAsciiAlpha(c) || c == '_';
}

bool isIdentifierChar(char c) {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool isHtmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool startsWithIgnoreCase(std::string_view s, size_t at, std::string_view prefix) {
    if (s.size() - at < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (lowerAscii(s[at + i]) != prefix[i]) return false;
    }
    return true;
}

}  // namespace

// Walks the text once, tracking just enough of the HTML tokenizer's state to
// know whether a position is in text: inside a tag (and inside a quoted
// attribute value within it), in a comment, or in a raw-text element.
struct TemplateParser {
    enum class State { Text, Tag, Comment, RawText };

    std::string_view src;
    ParsedTemplate out;
    State state = State::Text;
    char quote = 0;            // in Tag: the open quote character, if any
    std::string tagName;       // in Tag: the start tag's name ("" for end tags and declarations)
    bool tagIsEnd = false;
    std::string rawTextEnd;    // in RawText: the element whose end tag ends it
    std::string literal;       // text since the last field
    int line = 1;

    void error(const std::string& message) { out.errors.push_back({line, message}); }

    void flushLiteral() {
        if (literal.empty()) return;
        out.compiled.parts_.push_back({false, std::move(literal)});
        literal.clear();
    }

    std::string where() const {
        switch (state) {
            case State::Tag: return "inside a tag (fields in attributes arrive with M4's templates)";
            case State::Comment: return "inside an HTML comment";
            case State::RawText: return "inside <" + rawTextEnd + ">";
            case State::Text: break;
        }
        return "here";
    }

    // At "{{": parses a field and returns the index just past it, or reports
    // an error and returns the index just past the "{{".
    size_t field(size_t i) {
        size_t start = i;
        i += 2;
        if (state != State::Text) {
            error("a field isn't allowed " + where() + "; write \\{{ for a literal {{");
            return start + 2;
        }
        // "<{{x}}>" is text to the tokenizer, but x = "script" would make a tag.
        bool afterLt = !literal.empty() && literal.back() == '<';
        bool afterLtSlash = literal.size() >= 2 && literal.compare(literal.size() - 2, 2, "</") == 0;
        if (afterLt || afterLtSlash) {
            error("a field can't follow '<'; it could become a tag name");
            return start + 2;
        }
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) i++;
        size_t nameStart = i;
        if (i < src.size() && isIdentifierStart(src[i])) {
            while (i < src.size() && isIdentifierChar(src[i])) i++;
        }
        std::string name(src.substr(nameStart, i - nameStart));
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) i++;
        if (name.empty() || src.substr(i, 2) != "}}") {
            if (nameStart < src.size() && src[nameStart] == '>') {
                error("{{> includes}} aren't supported yet (they arrive with M4's templates)");
            } else if (!name.empty() && i < src.size() && src[i] == '.') {
                error("dotted field names like {{" + name + ".x}} aren't supported yet");
            } else {
                error("malformed field; expected {{name}}, or \\{{ for a literal {{");
            }
            return start + 2;
        }
        flushLiteral();
        out.compiled.parts_.push_back({true, name});
        auto& names = out.compiled.fieldNames_;
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        return i + 2;
    }

    void run() {
        size_t i = 0;
        while (i < src.size()) {
            char c = src[i];
            if (c == '\\' && src.substr(i + 1, 2) == "{{") {
                literal += "{{";
                i += 3;
                continue;
            }
            if (c == '{' && src.substr(i, 2) == "{{") {
                i = field(i);
                continue;
            }
            if (c == '\n') line++;
            literal += c;
            i++;

            switch (state) {
                case State::Text:
                    if (c == '<' && i < src.size()) {
                        char next = src[i];
                        if (src.substr(i, 3) == "!--") {
                            state = State::Comment;
                            literal += "!--";
                            i += 3;
                        } else if (isAsciiAlpha(next) || next == '/' || next == '!' || next == '?') {
                            state = State::Tag;
                            quote = 0;
                            tagIsEnd = next == '/';
                            tagName.clear();
                            size_t j = tagIsEnd ? i + 1 : i;
                            if (next != '!' && next != '?') {
                                while (j < src.size() && (isAsciiAlpha(src[j]) || (src[j] >= '0' && src[j] <= '9'))) {
                                    tagName += lowerAscii(src[j]);
                                    j++;
                                }
                            }
                        }
                    }
                    break;

                case State::Tag:
                    if (quote) {
                        if (c == quote) quote = 0;
                    } else if (c == '"' || c == '\'') {
                        quote = c;
                    } else if (c == '>') {
                        if (!tagIsEnd && isRawTextElement(tagName)) {
                            state = State::RawText;
                            rawTextEnd = tagName;
                        } else {
                            state = State::Text;
                        }
                    }
                    break;

                case State::Comment:
                    if (c == '>' && literal.size() >= 3 && literal.compare(literal.size() - 3, 3, "-->") == 0) {
                        state = State::Text;
                    }
                    break;

                case State::RawText:
                    // Ends at "</name" followed by a space, '/' or '>'.
                    if (c == '<' && i < src.size() && src[i] == '/' &&
                        startsWithIgnoreCase(src, i + 1, rawTextEnd)) {
                        size_t after = i + 1 + rawTextEnd.size();
                        if (after >= src.size() || isHtmlSpace(src[after]) || src[after] == '/' || src[after] == '>') {
                            state = State::Tag;
                            quote = 0;
                            tagIsEnd = true;
                            tagName = rawTextEnd;
                        }
                    }
                    break;
            }
        }
        flushLiteral();
    }
};

ParsedTemplate parseTemplate(const CodeFile& file) {
    TemplateParser parser;
    parser.src = file.contents();
    parser.run();
    return std::move(parser.out);
}

std::string escapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string renderTemplate(const CompiledTemplate& compiled, const TemplateFields& fields) {
    std::string out;
    for (const CompiledTemplate::Part& part : compiled.parts()) {
        if (!part.isField) {
            out += part.text;
            continue;
        }
        auto it = fields.find(part.text);
        if (it != fields.end() && it->second) out += escapeHtml(*it->second);
    }
    return out;
}

}  // namespace fourfold
