// Dynamic Markdown: a .md file in code/ as a template (overview §4, Fields in
// Markdown; docs/user/MARKDOWN.md).
//
// Parse first, fill second. Each {{...}} in the source is replaced by a
// placeholder (renderer.h), md4c parses the result once, and the renderer
// reports where each placeholder landed: in text, a link's destination, a
// block of its own, code, and so on. From that one parse come both forms:
//
// - The HTML form: the rendered HTML, split at the placeholders.
// - The Markdown form: the source itself, split at its fields, so a page with
//   no fields is sent exactly as written.
//
// Each field's context is fixed here, and a value is escaped for it when
// the template is filled, so a value can never change the structure of
// either output. Where a field can't be placed safely, it's a compile error.

#include <algorithm>

#include "markdown/markdown.h"
#include "renderer.h"

namespace fourfold {

namespace {

bool isIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentifierChar(char c) {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether s[at] is escaped by an odd run of backslashes before it.
bool escapedAt(std::string_view s, size_t at) {
    size_t n = 0;
    while (at > n && s[at - n - 1] == '\\') n++;
    return n % 2 == 1;
}

const char* const kFieldTip = "; write \\{{ for a literal {{";

// What s[i] starts or ends that would make a Markdown client link the text
// around it: "@", "://" or "www."; or nullptr.
const char* bareLinkAt(std::string_view s, size_t i) {
    if (s[i] != '@' && s[i] != ':' && s[i] != '.') return nullptr;
    if (escapedAt(s, i)) return nullptr;
    if (s[i] == '@') return "@";
    if (s[i] == ':') return s.compare(i + 1, 2, "//") == 0 ? "://" : nullptr;
    bool www = i >= 3 && lowerAscii(s[i - 1]) == 'w' && lowerAscii(s[i - 2]) == 'w' && lowerAscii(s[i - 3]) == 'w';
    return www ? "www." : nullptr;
}

// For each position in `s`: the last bare-link marker before it, and the
// first at or after it, in its run of text (up to the nearest spaces).
struct BareLinks {
    std::vector<const char*> before, after;

    explicit BareLinks(std::string_view s) : before(s.size() + 1), after(s.size() + 1) {
        const char* last = nullptr;
        for (size_t i = 0; i < s.size(); i++) {
            before[i] = last;
            if (isSpace(s[i])) {
                last = nullptr;
            } else if (const char* m = bareLinkAt(s, i)) {
                last = m;
            }
        }
        before[s.size()] = last;
        const char* next = nullptr;
        for (size_t i = s.size(); i-- > 0;) {
            if (isSpace(s[i])) {
                next = nullptr;
            } else if (const char* m = bareLinkAt(s, i)) {
                next = m;
            }
            after[i] = next;
        }
    }
};

}  // namespace

struct MarkdownTemplateBuilder : md::TemplateScan {
    using Part = CompiledTemplate::Part;
    using Landing = md::Landing;

    // A {{...}}, or a reserved [[, in the source.
    struct Token {
        enum class Kind { Field, Include, Bad, Reserved };
        Kind kind = Kind::Bad;
        size_t start = 0, end = 0;   // in the source
        int line = 0;
        std::vector<std::string> path;   // Field
        std::string name;                // Include
        std::string message;             // Bad: why
        std::string original;            // the source text
        bool afterLt = false;            // right after an unescaped '<'
        std::string bareLink;            // in a run of text a client would link ("@", ...)
        bool followedByDot = false;
        bool inRawHtml = false;          // inside raw HTML, as a GFM client would parse it
        size_t at = 0;                   // its placeholder's offset in `doc`
        std::vector<Landing> landings;
    };

    std::string_view src;
    std::string file;
    TemplateKind kind = TemplateKind::Fragment;
    std::string doc;   // the source with placeholders
    std::vector<Token> tokens;
    ParsedTemplate out;

    // --- TemplateScan ---

    std::string_view document() const override { return doc; }

    void landed(size_t index, Landing where) override {
        if (index >= tokens.size()) return;
        std::vector<Landing>& l = tokens[index].landings;
        if (where == Landing::Block && !l.empty() && l.back() == Landing::Text) {
            l.back() = Landing::Block;   // the paragraph it was text in is the field alone
        } else {
            l.push_back(where);
        }
    }

    const std::string& original(size_t index) const override {
        static const std::string kNone;
        return index < tokens.size() ? tokens[index].original : kNone;
    }

    void error(int line, std::string message) { out.errors.push_back({file, line, std::move(message)}); }

    // --- Finding the tokens ---

    // At "{{" (not escaped): the field or include there.
    Token readToken(size_t at, int line) const {
        Token t;
        t.start = at;
        t.end = at + 2;
        t.line = line;
        size_t j = at + 2;
        auto skipSpaces = [&] {
            while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) j++;
        };
        skipSpaces();
        if (j < src.size() && src[j] == '>') {
            j++;
            skipSpaces();
            size_t nameStart = j;
            while (j < src.size() && (isIdentifierChar(src[j]) || src[j] == '-' || src[j] == '/')) j++;
            std::string name(src.substr(nameStart, j - nameStart));
            skipSpaces();
            bool valid = !name.empty() && name.front() != '/' && name.back() != '/' &&
                         name.find("//") == std::string::npos;
            if (!valid || src.compare(j, 2, "}}") != 0) {
                t.message = "malformed include; expected {{> name}}, naming a file in code/frags/";
                return t;
            }
            t.kind = Token::Kind::Include;
            t.name = std::move(name);
            t.end = j + 2;
            return t;
        }
        std::vector<std::string> path;
        while (true) {
            size_t start = j;
            if (j < src.size() && isIdentifierStart(src[j])) {
                while (j < src.size() && isIdentifierChar(src[j])) j++;
            }
            if (j == start) break;
            path.emplace_back(src.substr(start, j - start));
            if (j < src.size() && src[j] == '.') {
                j++;
                continue;
            }
            break;
        }
        skipSpaces();
        if (path.empty() || src[j - 1] == '.' || src.compare(j, 2, "}}") != 0) {
            t.message = std::string("malformed field; expected {{name}} or {{name.key}}") + kFieldTip;
            return t;
        }
        t.kind = Token::Kind::Field;
        t.path = std::move(path);
        t.end = j + 2;
        return t;
    }

    void scanTokens() {
        BareLinks bareLinks(src);
        int line = 1;
        doc.reserve(src.size());
        for (size_t i = 0; i < src.size();) {
            bool braces = src.compare(i, 2, "{{") == 0;
            bool brackets = src.compare(i, 2, "[[") == 0;
            if ((braces || brackets) && escapedAt(src, i)) {
                doc.append(src.substr(i, 2));   // \{{ and \[[: literal, as Markdown escapes
                i += 2;
                continue;
            }
            if (!braces && !brackets) {
                if (src[i] == '\n') line++;
                doc += src[i++];
                continue;
            }
            Token t;
            if (braces) {
                t = readToken(i, line);
            } else {
                t.kind = Token::Kind::Reserved;
                t.start = i;
                t.end = i + 2;
                t.line = line;
            }
            t.original = std::string(src.substr(t.start, t.end - t.start));
            if (t.kind == Token::Kind::Field) {
                t.afterLt = t.start > 0 && src[t.start - 1] == '<' && !escapedAt(src, t.start - 1);
                const char* link = bareLinks.before[t.start] ? bareLinks.before[t.start] : bareLinks.after[t.end];
                t.bareLink = link ? link : "";
                t.followedByDot = t.end < src.size() && src[t.end] == '.';
            }
            t.at = doc.size();
            doc += md::placeholder(tokens.size());
            tokens.push_back(std::move(t));
            i = tokens.back().end;
        }
    }

    // Marks the fields that a GFM client, which renders raw HTML, would
    // read as part of an HTML tag or block.
    void findRawHtml() {
        struct Finder {
            MarkdownTemplateBuilder* b;
        } finder{this};
        MD_PARSER parser{};
        parser.flags = MD_DIALECT_GITHUB;   // raw HTML on
        parser.enter_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
        parser.leave_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
        parser.enter_span = [](MD_SPANTYPE, void*, void*) { return 0; };
        parser.leave_span = [](MD_SPANTYPE, void*, void*) { return 0; };
        parser.text = [](MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
            if (type != MD_TEXT_HTML) return 0;
            auto* b = static_cast<Finder*>(user)->b;
            md::replacePlaceholders(std::string_view(text, size), [b](size_t index) {
                if (index < b->tokens.size()) b->tokens[index].inRawHtml = true;
                return std::string();
            });
            return 0;
        };
        md_parse(doc.data(), static_cast<MD_SIZE>(doc.size()), &parser, &finder);
    }

    // For each token: whether the destination or title it's in belongs to an
    // inline link ("](" before it on its line), not a reference definition.
    std::vector<bool> inInlineLink;

    void findInlineLinks() {
        inInlineLink.assign(tokens.size(), false);
        size_t next = 0;
        bool seen = false;   // a "](" so far on this line
        for (size_t i = 0; i < doc.size() && next < tokens.size(); i++) {
            if (i == tokens[next].at) inInlineLink[next++] = seen;
            if (doc[i] == '\n') seen = false;
            if (doc[i] == '(' && i >= 1 && doc[i - 1] == ']' && !escapedAt(doc, i - 1)) seen = true;
        }
    }

    // --- Checking where each token landed ---

    // The token's one landing outside code, or nullopt (having reported an
    // error, unless it's only in code).
    std::optional<Landing> placement(size_t index) {
        const Token& t = tokens[index];
        std::vector<Landing> placed;
        for (Landing l : t.landings) {
            if (l != Landing::Code) placed.push_back(l);
        }
        bool inCode = placed.empty() && !t.landings.empty();
        if (inCode) return std::nullopt;   // text, as written
        auto has = [&](Landing l) { return std::find(placed.begin(), placed.end(), l) != placed.end(); };
        std::string what = t.kind == Token::Kind::Include ? "an include" : "a field";

        if (t.kind == Token::Kind::Reserved) {
            error(t.line, "[[ is reserved in Markdown files in code/ (for form inputs, later); write \\[[ for a "
                          "literal [[, or put it in `code`");
            return std::nullopt;
        }
        if (t.kind == Token::Kind::Bad) {
            error(t.line, t.message);
            return std::nullopt;
        }
        std::string label = t.original;
        if (placed.empty() || placed.size() > 1) {
            if (has(Landing::Autolink)) {
                error(t.line, label + ": " + what + " can't go in an autolink; write a link instead, like "
                                      "[text](https://example.com/{{x}})");
            } else {
                error(t.line, label + ": " + what + " can't go in a link reference definition ([label]: url) or a "
                                      "link's label ([text][label]); put the URL in the link itself, like "
                                      "[text]({{url}})");
            }
            return std::nullopt;
        }
        Landing l = placed[0];
        if (t.inRawHtml) {
            error(t.line, label + ": " + what + " can't go inside raw HTML: Fourfold shows the HTML as text, but a "
                                  "Markdown client might render it, with the value inside");
            return std::nullopt;
        }
        switch (l) {
            case Landing::Autolink:
                error(t.line, label + ": " + what + " can't go in an autolink; write a link instead, like "
                                      "[text](https://example.com/{{x}})");
                return std::nullopt;
            case Landing::Info:
                error(t.line, label + ": " + what + " can't go in a code block's info string" + kFieldTip);
                return std::nullopt;
            case Landing::Brackets:
                error(t.line, label + ": " + what + " can't go inside [brackets] that aren't a link (in the Markdown "
                                      "output, they could become one); write \\[ and \\] for literal brackets");
                return std::nullopt;
            case Landing::RefusedUrl:
                error(t.line, label + ": " + what + " is in a link whose URL isn't allowed (only http, https, mailto "
                                      "and relative URLs are)");
                return std::nullopt;
            default: break;
        }
        if (t.kind == Token::Kind::Include) {
            if (l != Landing::Block) {
                error(t.line, label + " must be a paragraph of its own: alone on its line, at the top level (not in "
                                      "a list, quote or table), with a blank line before and after it");
                return std::nullopt;
            }
            return l;
        }
        if (l == Landing::Text) {
            if (t.afterLt) {
                error(t.line, label + ": a field can't follow '<'; it could become a tag name" + kFieldTip);
                return std::nullopt;
            }
            if (!t.bareLink.empty()) {
                error(t.line, label + ": a field can't be part of a bare web or email address (text with \"" +
                                      t.bareLink + "\" in it), which a Markdown client would make a link; write a "
                                      "link, like [text](https://example.com/{{x}}), or escape the \"" +
                                      t.bareLink.substr(0, 1) + "\" with a backslash");
                return std::nullopt;
            }
        }
        if ((l == Landing::Title || l == Landing::UrlStart || l == Landing::UrlPart) && !inInlineLink[index]) {
            error(t.line, label + ": a field can't go in a link reference definition ([label]: url); put the URL "
                                  "in the link itself, like [text]({{url}}), with the link on one line");
            return std::nullopt;
        }
        return l;
    }

    TemplateField makeField(const Token& t, FieldContext context, const std::string& attribute = "") const {
        TemplateField f;
        f.path = t.path;
        f.context = context;
        f.file = file;
        f.line = t.line;
        f.attribute = attribute;
        f.followedByDot = t.followedByDot;
        return f;
    }

    static void appendLiteral(std::vector<Part>& parts, std::string_view text) {
        if (text.empty()) return;
        if (!parts.empty() && parts.back().kind == Part::Kind::Literal) {
            parts.back().text += text;
        } else {
            Part p;
            p.text = std::string(text);
            parts.push_back(std::move(p));
        }
    }

    // `text` split at its placeholders into Literal parts and `make`'s parts.
    template <typename Make>
    static void split(std::vector<Part>& parts, std::string_view text, Make make) {
        size_t literalStart = 0;
        for (size_t i = 0; i < text.size();) {
            size_t index = 0, length = 0;
            if (!md::readPlaceholder(text, i, index, length)) {
                i++;
                continue;
            }
            appendLiteral(parts, text.substr(literalStart, i - literalStart));
            make(index);
            i += length;
            literalStart = i;
        }
        appendLiteral(parts, text.substr(literalStart));
    }

    void run(const CodeFile& codeFile, const MarkdownOptions& options, std::vector<std::string>* warnings) {
        src = codeFile.contents();
        file = codeFile.path().str();
        if (md::hasNoncharacters(src)) {
            int line = 1;
            for (size_t i = 0; i + 2 < src.size(); i++) {
                if (src[i] == '\n') line++;
                if (md::hasNoncharacters(src.substr(i, 3))) break;
            }
            error(line, "contains a Unicode noncharacter (U+FDD0–U+FDEF), which Fourfold keeps for its own use; "
                        "remove it");
            return;
        }
        if (auto tooBig = checkMarkdownSize(src, options.maxBytes)) {
            error(0, *tooBig);
            return;
        }
        scanTokens();
        if (doc.size() > 0xFFFFFFF0u) {
            error(0, "the file is too long");
            return;
        }

        MarkdownOptions full = options;
        full.profile = MarkdownProfile::Full;
        md::Renderer r(full);
        r.scan = this;
        if (!r.run(doc)) {
            error(0, "the Markdown parser ran out of memory");
            return;
        }
        findRawHtml();
        findInlineLinks();

        std::vector<std::optional<Landing>> where(tokens.size());
        for (size_t i = 0; i < tokens.size(); i++) where[i] = placement(i);
        if (!out.ok()) return;
        if (warnings) warnings->insert(warnings->end(), r.result.warnings.begin(), r.result.warnings.end());

        CompiledTemplate& t = out.compiled;
        t.source_ = codeFile.path();
        t.kind_ = kind;
        t.isMarkdown_ = true;
        t.hasMarkdown_ = true;
        for (size_t i = 0; i < tokens.size(); i++) {
            const Token& token = tokens[i];
            if (token.kind != Token::Kind::Field || !where[i]) continue;
            if (std::find(t.fieldNames_.begin(), t.fieldNames_.end(), token.path[0]) == t.fieldNames_.end()) {
                t.fieldNames_.push_back(token.path[0]);
            }
        }

        // The HTML form.
        split(t.parts_, r.out, [&](size_t index) {
            const Token& token = tokens[index];
            Part p;
            p.line = token.line;
            if (token.kind == Token::Kind::Include) {
                p.kind = Part::Kind::Include;
                p.text = token.name;
            } else {
                p.kind = Part::Kind::Field;
                switch (*where[index]) {
                    case Landing::Block: p.field = makeField(token, FieldContext::MarkdownBlock); break;
                    case Landing::Alt: p.field = makeField(token, FieldContext::Attribute, "alt"); break;
                    case Landing::Title: p.field = makeField(token, FieldContext::Attribute, "title"); break;
                    case Landing::UrlStart: p.field = makeField(token, FieldContext::UrlStart, "href"); break;
                    case Landing::UrlPart: p.field = makeField(token, FieldContext::UrlPart, "href"); break;
                    default: p.field = makeField(token, FieldContext::MarkdownText); break;
                }
            }
            t.parts_.push_back(std::move(p));
        });

        // The Markdown form: the source, with each field's place marked.
        size_t literalStart = 0;
        for (size_t i = 0; i < tokens.size(); i++) {
            const Token& token = tokens[i];
            appendLiteral(t.markdownParts_, src.substr(literalStart, token.start - literalStart));
            literalStart = token.end;
            if (!where[i]) {   // in code: as written
                appendLiteral(t.markdownParts_, token.original);
                continue;
            }
            Part p;
            p.line = token.line;
            if (token.kind == Token::Kind::Include) {
                p.kind = Part::Kind::Include;
                p.text = token.name;
            } else {
                p.kind = Part::Kind::Field;
                switch (*where[i]) {
                    case Landing::Block: p.field = makeField(token, FieldContext::MarkdownBlock); break;
                    case Landing::UrlStart: p.field = makeField(token, FieldContext::UrlStart, "href"); break;
                    case Landing::UrlPart: p.field = makeField(token, FieldContext::UrlPart, "href"); break;
                    default: p.field = makeField(token, FieldContext::MarkdownText); break;
                }
            }
            t.markdownParts_.push_back(std::move(p));
        }
        appendLiteral(t.markdownParts_, src.substr(literalStart));

        // The title, as plain text and fields.
        if (kind == TemplateKind::Page) {
            split(t.titleParts_, r.result.title, [&](size_t index) {
                if (tokens[index].kind != Token::Kind::Field) return;
                Part p;
                p.kind = Part::Kind::Field;
                p.field = makeField(tokens[index], FieldContext::MarkdownText);
                t.titleParts_.push_back(std::move(p));
            });
        }
    }
};

ParsedTemplate parseMarkdownTemplate(const CodeFile& file, TemplateKind kind, const MarkdownOptions& options,
                                     std::vector<std::string>* warnings) {
    MarkdownTemplateBuilder b;
    b.kind = kind;
    b.run(file, options, warnings);
    return std::move(b.out);
}

}  // namespace fourfold
