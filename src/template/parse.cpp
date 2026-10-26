// The template parser: an HTML tokenizer, following the spec's tokenizer
// states closely enough to know, at every field, which context it's in.
//
// It tracks text, tags (with their attributes, read in full before any is
// classified), comments, declarations, raw-text and RCDATA elements, and
// foreign content (<svg>, <math>). Where browsers' parsing gets subtle, the
// rule is to refuse rather than guess: see the compile errors below, and
// docs/design/m4-results.md for why each exists.

#include <algorithm>

#include "js_lexer.h"
#include "template/template.h"

namespace fourfold {

namespace {

// Elements whose content the tokenizer reads as raw text, or (for textarea
// and title) as RCDATA, where character references work but tags don't.
const char* const kRawTextElements[] = {"style", "xmp", "iframe", "noembed", "noframes", "noscript"};
const char* const kRcdataElements[] = {"textarea", "title"};

// HTML's boolean attributes: present means on, whatever the value.
const char* const kBooleanAttributes[] = {
    "allowfullscreen", "async",    "autofocus", "autoplay",  "checked",  "controls", "default",
    "defer",           "disabled", "formnovalidate", "hidden", "inert",  "ismap",    "itemscope",
    "loop",            "multiple", "muted",     "nomodule",  "novalidate", "open",   "playsinline",
    "readonly",        "required", "reversed",  "selected"};

// Attributes whose value is a URL (html/template's list, plus HTMX's).
const char* const kUrlAttributes[] = {
    "action",  "archive", "background", "cite",       "classid",     "codebase",  "data",
    "formaction", "href", "icon",       "longdesc",   "manifest",    "ping",      "poster",
    "profile", "src",     "usemap",     "xmlns",      "xlink:href",  "hx-get",    "hx-post",
    "hx-put",  "hx-patch", "hx-delete", "hx-push-url", "hx-replace-url", "sse-connect", "ws-connect"};

// Attributes whose literal URLs the compiler checks: ones a browser follows
// or loads (with a data- prefix ignored only for HTMX's).
const char* const kLinkAttributes[] = {"href",      "src",        "action",       "formaction",   "poster",
                                       "hx-get",    "hx-post",    "hx-put",       "hx-patch",     "hx-delete",
                                       "hx-push-url", "hx-replace-url", "sse-connect"};

// Start tags that end foreign content early in browsers ("breakout" tags):
// <svg><p> closes the svg. Refused inside <svg> and <math>.
const char* const kBreakoutElements[] = {
    "b",    "big",  "blockquote", "body",  "br",     "center", "code",  "dd",    "div",  "dl",   "dt",
    "em",   "embed", "h1",        "h2",    "h3",     "h4",     "h5",    "h6",    "head", "hr",   "i",
    "img",  "li",   "listing",    "menu",  "meta",   "nobr",   "ol",    "p",     "pre",  "ruby", "s",
    "small", "span", "strong",    "strike", "sub",   "sup",    "table", "tt",    "u",    "ul",   "var",
    "font"};

template <size_t N>
bool isOneOf(const std::string& name, const char* const (&list)[N]) {
    for (const char* e : list) {
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

std::string lower(std::string_view s) {
    std::string out;
    for (char c : s) out += lowerAscii(c);
    return out;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

bool startsWithIgnoreCase(std::string_view s, size_t at, std::string_view prefix) {
    if (at > s.size() || s.size() - at < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (lowerAscii(s[at + i]) != prefix[i]) return false;
    }
    return true;
}

bool containsIgnoreCase(const std::string& haystack, std::string_view needle) {
    std::string h = lower(haystack);
    return h.find(needle) != std::string::npos;
}

bool isUrlAttribute(const std::string& name) {
    if (isOneOf(name, kUrlAttributes)) return true;
    return name.find("src") != std::string::npos || name.find("uri") != std::string::npos ||
           name.find("url") != std::string::npos;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isHtmlSpace(s[a])) a++;
    while (b > a && isHtmlSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

const char* const kFieldTip = "; write \\{{ for a literal {{";

}  // namespace

const char* fieldContextName(FieldContext context) {
    switch (context) {
        case FieldContext::Text: return "text";
        case FieldContext::Rcdata: return "rcdata";
        case FieldContext::Attribute: return "attribute";
        case FieldContext::UrlStart: return "url";
        case FieldContext::UrlPart: return "url-part";
        case FieldContext::Script: return "script";
        case FieldContext::Boolean: return "boolean";
    }
    return "?";
}

std::string TemplateField::label() const {
    std::string s;
    for (const std::string& p : path) {
        if (!s.empty()) s += '.';
        s += p;
    }
    return s;
}

CompiledTemplate CompiledTemplate::fromMarkdown(RelPath source, Html html) {
    CompiledTemplate t;
    t.source_ = std::move(source);
    t.kind_ = TemplateKind::Fragment;
    t.isMarkdown_ = true;
    t.hasMarkdown_ = true;
    CompiledTemplate::Part part;
    part.text = html.str();
    t.parts_.push_back(std::move(part));
    return t;
}

struct TemplateParser {
    using Part = CompiledTemplate::Part;

    // A {{...}} token.
    struct Token {
        enum class Kind { Field, Include, Bad };
        Kind kind = Kind::Bad;
        std::vector<std::string> path;   // Field
        std::string name;                // Include
        size_t end = 0;                  // just past it
    };

    // A piece of an attribute's value: literal text, or a field.
    struct ValuePiece {
        bool isField = false;
        std::string text;
        std::vector<std::string> path;
        int line = 0;
    };

    struct Attribute {
        std::string leading;   // the whitespace before it
        std::string rawName;   // as written
        std::string name;      // lowercased
        bool hasValue = false;
        std::string eq;        // from the end of the name through the opening quote: `="`
        char quote = 0;        // 0 if unquoted (or no value)
        std::vector<ValuePiece> value;
        std::string closing;   // the closing quote
        bool fieldRefused = false;   // a field here was already reported
        int line = 0;

        bool hasFields() const {
            for (const ValuePiece& p : value) {
                if (p.isField) return true;
            }
            return false;
        }
        std::string literalValue() const {
            std::string s;
            for (const ValuePiece& p : value) {
                if (!p.isField) s += p.text;
            }
            return s;
        }
    };

    struct Tag {
        std::string rawName;
        std::string name;
        std::vector<Attribute> attributes;
        std::string tail;   // whitespace, then ">" or "/>"
        bool selfClosing = false;
        bool closed = false;
        int line = 0;

        const Attribute* find(const char* attrName) const {
            for (const Attribute& a : attributes) {
                if (a.name == attrName) return &a;
            }
            return nullptr;
        }
    };

    std::string_view src;
    std::string file;
    TemplateKind kind = TemplateKind::Fragment;
    ParsedTemplate out;

    size_t i = 0;
    int line = 1;
    std::string literal;   // text since the last part

    bool seenSignificant = false;   // for a page: has anything but whitespace and comments come yet?
    bool shapeReported = false;
    bool headEndPlaced = false;
    int foreignDepth = 0;           // open <svg> and <math> elements
    std::string integration;        // inside <svg>'s <title> or <desc>

    CompiledTemplate& compiled() { return out.compiled; }

    void error(int at, const std::string& message) { out.errors.push_back({file, at, message}); }

    char take() {
        char c = src[i++];
        if (c == '\n') line++;
        return c;
    }
    bool more() const { return i < src.size(); }
    char peek(size_t ahead = 0) const { return i + ahead < src.size() ? src[i + ahead] : '\0'; }
    bool atEscapedBraces() const { return src.compare(i, 3, "\\{{") == 0; }
    bool atBraces() const { return src.compare(i, 2, "{{") == 0; }

    void flush() {
        if (literal.empty()) return;
        auto& parts = compiled().parts_;
        if (!parts.empty() && parts.back().kind == Part::Kind::Literal) {
            parts.back().text += literal;
        } else {
            Part p;
            p.text = std::move(literal);
            parts.push_back(std::move(p));
        }
        literal.clear();
    }

    void push(Part part) {
        flush();
        compiled().parts_.push_back(std::move(part));
    }

    void noteFieldName(const std::string& name) {
        auto& names = compiled().fieldNames_;
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }

    TemplateField makeField(std::vector<std::string> path, FieldContext context, int at,
                            const std::string& attribute = "") {
        noteFieldName(path[0]);
        TemplateField f;
        f.path = std::move(path);
        f.context = context;
        f.file = file;
        f.line = at;
        f.attribute = attribute;
        return f;
    }

    void pushField(std::vector<std::string> path, FieldContext context, int at, const std::string& attribute = "") {
        Part p;
        p.kind = Part::Kind::Field;
        p.field = makeField(std::move(path), context, at, attribute);
        push(std::move(p));
    }

    // At "{{": reads the field or include. A Bad token is reported here.
    Token readToken(size_t at) {
        Token t;
        size_t j = at + 2;
        t.end = j;
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
                error(line, "malformed include; expected {{> name}}, naming a file in code/frags/");
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
        if (path.empty() || (j > 0 && src[j - 1] == '.') || src.compare(j, 2, "}}") != 0) {
            error(line, std::string("malformed field; expected {{name}} or {{name.key}}") + kFieldTip);
            return t;
        }
        t.kind = Token::Kind::Field;
        t.path = std::move(path);
        t.end = j + 2;
        return t;
    }

    // At "{{" where no field may go: reports it and skips it.
    void refuseField(const std::string& why) {
        int at = line;
        Token t = readToken(i);
        if (t.kind != Token::Kind::Bad) {
            error(at, (t.kind == Token::Kind::Include ? "an include isn't allowed " : "a field isn't allowed ") + why +
                          kFieldTip);
        }
        i = t.end;
    }

    // For a page: the first thing that isn't whitespace or a comment must
    // be the doctype or <html>.
    void significant(bool documentStart) {
        if (kind == TemplateKind::Page && !seenSignificant && !documentStart && !shapeReported) {
            error(line, "a page must be a complete document, starting with <!DOCTYPE html> (pieces of pages "
                        "go in code/frags/)");
            shapeReported = true;
        }
        seenSignificant = true;
    }

    // --- Text ---

    void textField() {
        int at = line;
        if (foreignDepth > 0) {
            refuseField("inside <svg> or <math>");
            return;
        }
        Token t = readToken(i);
        i = t.end;
        if (t.kind == Token::Kind::Bad) return;
        significant(false);
        if (t.kind == Token::Kind::Include) {
            Part p;
            p.kind = Part::Kind::Include;
            p.text = t.name;
            p.line = at;
            push(std::move(p));
            return;
        }
        // "<{{x}}" is text to the tokenizer, but x = "script" would make a tag.
        bool afterLt = !literal.empty() && literal.back() == '<';
        if (afterLt) {
            error(at, "a field can't follow '<'; it could become a tag name");
            return;
        }
        pushField(std::move(t.path), FieldContext::Text, at);
    }

    // --- Comments and declarations ---

    // At "<!--".
    void comment() {
        int start = line;
        for (int k = 0; k < 4; k++) literal += take();
        // "<!-->" and "<!--->" are complete comments.
        if (peek() == '>') {
            literal += take();
            return;
        }
        if (peek() == '-' && peek(1) == '>') {
            literal += take();
            literal += take();
            return;
        }
        while (true) {
            if (!more()) {
                error(start, "unclosed comment: <!-- with no -->");
                return;
            }
            if (atEscapedBraces()) {
                literal += "{{";
                i += 3;
            } else if (atBraces()) {
                refuseField("inside an HTML comment");
            } else if (src.compare(i, 3, "-->") == 0) {
                for (int k = 0; k < 3; k++) literal += take();
                return;
            } else if (src.compare(i, 4, "--!>") == 0) {
                for (int k = 0; k < 4; k++) literal += take();
                return;
            } else {
                literal += take();
            }
        }
    }

    // Copies through `terminator`, refusing fields. Already past the opening.
    void copyThrough(std::string_view terminator, const char* what, int start) {
        while (true) {
            if (!more()) {
                error(start, std::string("unclosed ") + what);
                return;
            }
            if (atEscapedBraces()) {
                literal += "{{";
                i += 3;
            } else if (atBraces()) {
                refuseField(std::string("inside ") + what);
            } else if (src.compare(i, terminator.size(), terminator) == 0) {
                for (size_t k = 0; k < terminator.size(); k++) literal += take();
                return;
            } else {
                literal += take();
            }
        }
    }

    // At "<!" (not a comment) or "<?".
    void declaration() {
        int start = line;
        if (startsWithIgnoreCase(src, i, "<!doctype")) {
            if (kind == TemplateKind::Fragment) {
                error(start, "a fragment can't have a doctype; whole documents go in code/pages/");
            }
            significant(true);
            copyThrough(">", "<!DOCTYPE>", start);
            return;
        }
        significant(false);
        if (foreignDepth > 0 && src.compare(i, 9, "<![CDATA[") == 0) {
            for (int k = 0; k < 9; k++) literal += take();
            copyThrough("]]>", "<![CDATA[ section", start);
            return;
        }
        // A bogus comment, to the next '>'.
        literal += take();
        copyThrough(">", "<!...> or <?...> declaration", start);
    }

    // --- Tags ---

    // Reads attributes up to and including the tag's end. Fields are kept in
    // attribute values (for start tags); anywhere else they're refused.
    void readAttributes(Tag& tag, bool isEnd) {
        std::string pending;   // whitespace not yet given to an attribute
        while (true) {
            if (!more()) {
                error(tag.line, "unclosed tag <" + tag.rawName + ">");
                return;
            }
            char c = peek();
            if (isHtmlSpace(c)) {
                pending += take();
                continue;
            }
            if (c == '>') {
                take();
                tag.tail = pending + ">";
                tag.closed = true;
                return;
            }
            if (c == '/') {
                if (peek(1) == '>') {
                    take();
                    take();
                    tag.tail = pending + "/>";
                    tag.selfClosing = true;
                    tag.closed = true;
                    return;
                }
                pending += take();
                continue;
            }

            Attribute a;
            a.leading = std::move(pending);
            pending.clear();
            a.line = line;
            // The name. Its first character may be '='.
            bool first = true;
            while (more() && !isHtmlSpace(peek()) && peek() != '/' && peek() != '>' && (first || peek() != '=')) {
                first = false;
                if (atBraces()) {
                    refuseField("in an attribute name; put it in a quoted value, like title=\"{{x}}\"");
                    a.fieldRefused = true;
                    continue;
                }
                a.rawName += take();
            }
            a.name = lower(a.rawName);

            std::string ws;
            while (more() && isHtmlSpace(peek())) ws += take();
            if (peek() != '=') {
                pending = std::move(ws);
                tag.attributes.push_back(std::move(a));
                continue;
            }
            a.hasValue = true;
            a.eq = ws + take();
            while (more() && isHtmlSpace(peek())) a.eq += take();

            auto addText = [&](const std::string& text) {
                if (a.value.empty() || a.value.back().isField) a.value.push_back(ValuePiece{});
                a.value.back().text += text;
            };

            if (peek() == '"' || peek() == '\'') {
                a.quote = take();
                a.eq += a.quote;
                while (true) {
                    if (!more()) {
                        error(a.line, "unclosed attribute value in <" + tag.rawName + ">");
                        tag.attributes.push_back(std::move(a));
                        return;
                    }
                    if (peek() == a.quote) {
                        a.closing = std::string(1, take());
                        break;
                    }
                    if (atEscapedBraces()) {
                        addText("{{");
                        i += 3;
                    } else if (atBraces()) {
                        if (isEnd) {
                            refuseField("in an end tag");
                            continue;
                        }
                        int at = line;
                        Token t = readToken(i);
                        i = t.end;
                        if (t.kind == Token::Kind::Include) {
                            error(at, "an include can only go in text, not in an attribute");
                        } else if (t.kind == Token::Kind::Field) {
                            ValuePiece piece;
                            piece.isField = true;
                            piece.path = std::move(t.path);
                            piece.line = at;
                            a.value.push_back(std::move(piece));
                        }
                    } else {
                        addText(std::string(1, take()));
                    }
                }
            } else {
                // Unquoted: ends at whitespace or '>'.
                while (more() && !isHtmlSpace(peek()) && peek() != '>') {
                    if (atEscapedBraces()) {
                        addText("{{");
                        i += 3;
                    } else if (atBraces()) {
                        refuseField("in an unquoted attribute value; put quotes around it, like title=\"{{x}}\"");
                        a.fieldRefused = true;
                    } else {
                        addText(std::string(1, take()));
                    }
                }
            }
            tag.attributes.push_back(std::move(a));
        }
    }

    // What context the fields of `a` are in, or nullopt (with the error
    // reported) if fields aren't allowed there.
    std::optional<FieldContext> attributeContext(const Tag& tag, const Attribute& a) {
        const std::string& n = a.name;
        std::string bare = startsWith(n, "data-") ? n.substr(5) : n;
        auto refuse = [&](const std::string& why) {
            error(a.line, "a field isn't allowed in " + why);
            return std::nullopt;
        };
        if (foreignDepth > 0) return refuse("attributes inside <svg> or <math>");
        if (a.fieldRefused) return std::nullopt;
        if (n == "style") return refuse("a style attribute; choose a class in code instead, like class=\"{{cls}}\"");
        if (n == "srcset") return refuse("srcset; use src=\"{{url}}\"");
        if (n == "srcdoc") return refuse("srcdoc, which is a whole HTML document");
        if (tag.name == "base") return refuse("<base>, which changes what every URL on the page means");
        if (tag.name == "meta" && tag.find("http-equiv")) {
            return refuse("a <meta http-equiv> tag, which can redirect or set headers");
        }
        if (bare == "hx-vals" || bare == "hx-headers" || bare == "hx-trigger" || bare == "hx-vars") {
            return refuse(n + ", which HTMX can evaluate as JavaScript");
        }
        if (isOneOf(n, kBooleanAttributes)) {
            if (a.value.size() != 1 || !a.value[0].isField) {
                error(a.line, n + " is a boolean attribute, so its value must be one field and nothing else, like " +
                                  n + "=\"{{x}}\"");
                return std::nullopt;
            }
            return FieldContext::Boolean;
        }
        if (isUrlAttribute(bare)) return FieldContext::UrlStart;   // refined per field
        return FieldContext::Attribute;
    }

    // Checks that apply to attributes whether or not they have fields.
    void checkAttribute(const Attribute& a) {
        const std::string& n = a.name;
        std::string bare = startsWith(n, "data-") ? n.substr(5) : n;
        if (startsWith(n, "on")) {
            error(a.line, "inline event handlers like " + a.rawName +
                              "= aren't allowed: the Content Security Policy would block them. Instead, mark the "
                              "element (say, data-action=\"save\") and attach the handler in a .js file, with "
                              "addEventListener(\"" + lower(a.rawName.substr(2)) + "\", ...); or use HTMX "
                              "attributes. See docs/user/converting.md");
        } else if (startsWith(bare, "hx-on")) {
            error(a.line, a.rawName + " runs inline JavaScript, which the Content Security Policy blocks");
        } else if (bare == "hx-vars") {
            error(a.line, "hx-vars evaluates JavaScript, which the Content Security Policy blocks; use hx-vals "
                          "with plain JSON");
        } else if (n == "nonce") {
            error(a.line, "don't write nonce attributes: the engine adds a fresh nonce to every <script>");
        } else if (bare == "hx-vals" || bare == "hx-headers") {
            std::string v = lower(trimmed(a.literalValue()));
            if (startsWith(v, "js:") || startsWith(v, "javascript:")) {
                error(a.line, a.rawName + " with js: evaluates JavaScript, which the Content Security Policy "
                                          "blocks; use plain JSON");
            }
        }
    }

    // Records a literal URL in a link attribute, for the compiler's check.
    void noteLink(const Tag& tag, const Attribute& a) {
        if (!a.hasValue || a.hasFields() || a.fieldRefused || foreignDepth > 0) return;
        std::string n = startsWith(a.name, "data-hx-") ? a.name.substr(5) : a.name;
        bool isLink = isOneOf(n, kLinkAttributes) || (n == "data" && tag.name == "object");
        if (!isLink) return;
        std::string url = trimmed(a.literalValue());
        for (size_t at = url.find("&amp;"); at != std::string::npos; at = url.find("&amp;", at + 1)) {
            url.replace(at, 5, "&");
        }
        if (url.empty()) return;
        compiled().links_.push_back({url, n, file, a.line});
    }

    void emitStartTag(const Tag& tag) {
        if (tag.name == "body" && !headEndPlaced && foreignDepth == 0) {
            Part p;
            p.kind = Part::Kind::HeadEnd;
            push(std::move(p));
            headEndPlaced = true;
        }
        literal += "<" + tag.rawName;
        bool isScript = tag.name == "script" && foreignDepth == 0;
        if (isScript) {
            Part p;
            p.kind = Part::Kind::Nonce;
            push(std::move(p));
            compiled().hasScript_ = true;
        }
        for (const Attribute& a : tag.attributes) {
            checkAttribute(a);
            noteLink(tag, a);
            if (!a.hasFields()) {
                literal += a.leading + a.rawName + a.eq;
                for (const ValuePiece& piece : a.value) literal += piece.text;
                literal += a.closing;
                continue;
            }
            std::optional<FieldContext> context = attributeContext(tag, a);
            if (!context) {
                literal += a.leading + a.rawName + a.eq + a.closing;
                continue;
            }
            if (a.value.size() == 1 && a.quote) {
                // The whole value is one field: a null leaves the attribute out.
                const ValuePiece& piece = a.value[0];
                Part p;
                p.kind = Part::Kind::Attribute;
                p.text = a.leading + a.rawName;
                p.between = a.eq;
                p.after = a.closing;
                p.field = makeField(piece.path, *context, piece.line, a.name);
                push(std::move(p));
                continue;
            }
            literal += a.leading + a.rawName + a.eq;
            bool start = true;   // nothing but whitespace yet
            for (const ValuePiece& piece : a.value) {
                if (!piece.isField) {
                    literal += piece.text;
                    if (!trimmed(piece.text).empty()) start = false;
                    continue;
                }
                FieldContext c = *context;
                if (c == FieldContext::UrlStart && !start) c = FieldContext::UrlPart;
                pushField(piece.path, c, piece.line, a.name);
                start = false;
            }
            literal += a.closing;
        }
        literal += tag.tail;

        if (tag.name == "form" && foreignDepth == 0) {
            const Attribute* method = tag.find("method");
            if (method && lower(trimmed(method->literalValue())) == "post") {
                Part p;
                p.kind = Part::Kind::PostForm;
                push(std::move(p));
                compiled().postForms_++;
            }
        }
    }

    // At '<' followed by a letter.
    void startTag() {
        Tag tag;
        tag.line = line;
        take();   // '<'
        while (more() && !isHtmlSpace(peek()) && peek() != '/' && peek() != '>') {
            if (atBraces()) {
                refuseField("in a tag name");
                continue;
            }
            tag.rawName += take();
        }
        tag.name = lower(tag.rawName);
        readAttributes(tag, false);
        significant(tag.name == "html");

        if (kind == TemplateKind::Fragment &&
            (tag.name == "html" || tag.name == "head" || tag.name == "body")) {
            error(tag.line, "a fragment can't have <" + tag.name + ">; whole documents go in code/pages/");
        }
        if (foreignDepth > 0) {
            if (!integration.empty()) {
                error(tag.line, "tags inside <svg>'s <" + integration + "> aren't supported; keep it plain text");
            } else if (isOneOf(tag.name, kBreakoutElements)) {
                error(tag.line, "<" + tag.name + "> inside <svg> or <math> ends it early in browsers; close it first");
            } else if (tag.name == "script" || tag.name == "style" || tag.name == "plaintext" ||
                       tag.name == "foreignobject" || tag.name == "annotation-xml" ||
                       isOneOf(tag.name, kRawTextElements) || tag.name == "textarea") {
                error(tag.line, "<" + tag.name + "> inside <svg> or <math> isn't supported");
            }
        }
        if (tag.name == "meta") {
            const Attribute* name = tag.find("name");
            if (name && lower(trimmed(name->literalValue())) == "htmx-config") {
                error(tag.line, "htmx's settings are fixed by the engine (no eval, no scripts in swapped "
                                "content); a <meta name=\"htmx-config\"> can't change them");
            }
        }
        if (tag.name == "plaintext") {
            error(tag.line, "<plaintext> isn't supported: nothing after it would be HTML");
        }
        emitStartTag(tag);
        if (!tag.closed) return;

        if (foreignDepth > 0) {
            if ((tag.name == "svg" || tag.name == "math") && !tag.selfClosing) foreignDepth++;
            if ((tag.name == "title" || tag.name == "desc") && !tag.selfClosing) integration = tag.name;
            return;
        }
        if ((tag.name == "svg" || tag.name == "math") && !tag.selfClosing) {
            foreignDepth = 1;
            return;
        }
        // In HTML, "/>" doesn't close these: their content follows either way.
        if (tag.name == "script") {
            scriptContent(tag);
        } else if (isOneOf(tag.name, kRawTextElements)) {
            rawText(tag.name, false);
        } else if (isOneOf(tag.name, kRcdataElements)) {
            rawText(tag.name, true);
        }
    }

    // At "</" followed by a letter.
    void endTag() {
        Tag tag;
        tag.line = line;
        take();
        take();
        while (more() && !isHtmlSpace(peek()) && peek() != '/' && peek() != '>') {
            if (atBraces()) {
                refuseField("in an end tag");
                continue;
            }
            tag.rawName += take();
        }
        tag.name = lower(tag.rawName);
        significant(false);
        if (kind == TemplateKind::Fragment &&
            (tag.name == "html" || tag.name == "head" || tag.name == "body")) {
            error(tag.line, "a fragment can't have </" + tag.name + ">; whole documents go in code/pages/");
        }
        if (tag.name == "head" && !headEndPlaced && foreignDepth == 0) {
            Part p;
            p.kind = Part::Kind::HeadEnd;
            push(std::move(p));
            headEndPlaced = true;
        }
        literal += "</" + tag.rawName;
        readAttributes(tag, true);
        // End tags' attributes mean nothing; copy them through.
        for (const Attribute& a : tag.attributes) {
            literal += a.leading + a.rawName + a.eq;
            for (const ValuePiece& piece : a.value) literal += piece.text;
            literal += a.closing;
        }
        literal += tag.tail;

        if (foreignDepth > 0) {
            if (tag.name == integration) integration.clear();
            if (tag.name == "svg" || tag.name == "math") foreignDepth--;
        }
    }

    // Does an end tag for `name` start here ("</name" then space, '/' or '>')?
    bool atEndTagFor(const std::string& name) const {
        if (!(peek() == '<' && peek(1) == '/' && startsWithIgnoreCase(src, i + 2, name))) return false;
        size_t after = i + 2 + name.size();
        return after >= src.size() || isHtmlSpace(src[after]) || src[after] == '/' || src[after] == '>';
    }

    // The content of a raw-text element (fields refused) or an RCDATA one
    // (fields escaped), up to its end tag.
    void rawText(const std::string& name, bool rcdata) {
        int start = line;
        while (true) {
            if (!more()) {
                error(start, "unclosed <" + name + ">");
                return;
            }
            if (atEndTagFor(name)) return;
            if (atEscapedBraces()) {
                literal += "{{";
                i += 3;
            } else if (atBraces()) {
                if (!rcdata) {
                    refuseField("inside <" + name + ">" +
                                (name == "style" ? std::string("; choose a class or a stylesheet in code instead")
                                                 : std::string()));
                    continue;
                }
                int at = line;
                Token t = readToken(i);
                i = t.end;
                if (t.kind == Token::Kind::Include) {
                    error(at, "an include can only go in text, not inside <" + name + ">");
                } else if (t.kind == Token::Kind::Field) {
                    pushField(std::move(t.path), FieldContext::Rcdata, at);
                }
            } else {
                literal += take();
            }
        }
    }

    // A <script>'s content. Fields are allowed only where a JavaScript (or
    // JSON) value can go.
    void scriptContent(const Tag& tag) {
        int start = line;
        std::string type;
        if (const Attribute* t = tag.find("type")) type = lower(trimmed(t->literalValue()));
        bool fieldsAllowed = type.empty() || type == "text/javascript" || type == "application/javascript" ||
                             type == "module" || type == "application/json" || type == "application/ld+json" ||
                             type == "importmap";
        JsLexer js;
        std::string content;
        while (true) {
            if (!more()) {
                error(start, "unclosed <script>");
                break;
            }
            if (atEndTagFor("script")) break;
            if (atEscapedBraces()) {
                literal += "{{";
                content += "{{";
                js.feed("{{");
                i += 3;
            } else if (atBraces()) {
                int at = line;
                Token t = readToken(i);
                i = t.end;
                if (t.kind == Token::Kind::Include) {
                    error(at, "an include can only go in text, not inside <script>");
                } else if (t.kind == Token::Kind::Field) {
                    std::string why;
                    if (!fieldsAllowed) {
                        error(at, "a field is only allowed in a JavaScript or JSON <script>, not type=\"" + type +
                                      "\"");
                    } else if (!js.field(why)) {
                        error(at, "a field isn't allowed here: " + why +
                                      ". In a <script>, a field is a whole JSON value, like const x = {{x}};");
                    } else {
                        pushField(std::move(t.path), FieldContext::Script, at);
                    }
                }
                content += ' ';
            } else {
                char c = take();
                literal += c;
                content += c;
                js.feed(c);
            }
        }
        // Inside a script, "<!--" and "<script" switch the tokenizer into its
        // escaped states, where "</script>" may not end the element. Rather
        // than follow them, refuse them.
        if (containsIgnoreCase(content, "<!--") || containsIgnoreCase(content, "<script")) {
            error(start, "a <script> can't contain \"<!--\" or \"<script\" (browsers then read its end "
                         "differently); split the string, like \"<scr\" + \"ipt\"");
        }
    }

    void run(const RelPath& source) {
        compiled().source_ = source;
        compiled().kind_ = kind;
        if (src.find('\0') != std::string_view::npos) {
            error(1, "the file contains a NUL byte");
            return;
        }
        if (src.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            for (int k = 0; k < 3; k++) literal += take();
        }
        while (more()) {
            if (atEscapedBraces()) {
                significant(false);
                literal += "{{";
                i += 3;
                continue;
            }
            if (atBraces()) {
                textField();
                continue;
            }
            char c = peek();
            if (c == '<') {
                char next = peek(1);
                if (src.compare(i, 4, "<!--") == 0) {
                    comment();
                    continue;
                }
                if (next == '!' || next == '?') {
                    declaration();
                    continue;
                }
                if (next == '/') {
                    char after = peek(2);
                    if (isAsciiAlpha(after)) {
                        endTag();
                        continue;
                    }
                    if (after == '>') {   // "</>" is dropped by browsers
                        for (int k = 0; k < 3; k++) literal += take();
                        continue;
                    }
                    if (i + 2 < src.size()) {   // any other "</" starts a bogus comment
                        int start = line;
                        significant(false);
                        literal += take();
                        literal += take();
                        copyThrough(">", "</...> (not an end tag)", start);
                        continue;
                    }
                }
                if (isAsciiAlpha(next)) {
                    startTag();
                    continue;
                }
            }
            if (!isHtmlSpace(c)) significant(false);
            literal += take();
        }
        if (foreignDepth > 0) error(line, "unclosed <svg> or <math>");
        if (kind == TemplateKind::Page && !seenSignificant) {
            error(1, "a page must be a complete document, starting with <!DOCTYPE html>");
        }
        flush();
    }
};

ParsedTemplate parseTemplate(const CodeFile& file, TemplateKind kind) {
    TemplateParser parser;
    parser.src = file.contents();
    parser.file = file.path().str();
    parser.kind = kind;
    parser.run(file.path());
    return std::move(parser.out);
}

// --- Includes ---

struct IncludeResolver {
    using Part = CompiledTemplate::Part;

    std::vector<TemplateProblem> problems;

    // Splices included fragments into `t`, which must not be one of them.
    // `get` returns a resolved fragment, or null (having reported why).
    template <typename Get>
    void splice(CompiledTemplate& t, Get get) {
        std::vector<Part> parts;
        auto appendLiteral = [&](const std::string& text) {
            if (!parts.empty() && parts.back().kind == Part::Kind::Literal) {
                parts.back().text += text;
            } else {
                Part p;
                p.text = text;
                parts.push_back(std::move(p));
            }
        };
        for (Part& part : t.parts_) {
            if (part.kind != Part::Kind::Include) {
                if (part.kind == Part::Kind::Literal) {
                    appendLiteral(part.text);
                } else {
                    parts.push_back(std::move(part));
                }
                continue;
            }
            const CompiledTemplate* frag = get(part.text, t.source_.str(), part.line);
            if (!frag) continue;
            for (const Part& p : frag->parts_) {
                if (p.kind == Part::Kind::Literal) {
                    appendLiteral(p.text);
                } else if (p.kind == Part::Kind::HeadEnd) {
                    continue;   // a fragment has no head
                } else {
                    parts.push_back(p);
                }
            }
            for (const std::string& name : frag->fieldNames_) {
                if (std::find(t.fieldNames_.begin(), t.fieldNames_.end(), name) == t.fieldNames_.end()) {
                    t.fieldNames_.push_back(name);
                }
            }
            t.hasScript_ = t.hasScript_ || frag->hasScript_;
            t.hasMarkdown_ = t.hasMarkdown_ || frag->hasMarkdown_;
            t.postForms_ += frag->postForms_;
            t.links_.insert(t.links_.end(), frag->links_.begin(), frag->links_.end());
        }
        t.parts_ = std::move(parts);
    }

    void missing(const std::string& name, const std::string& file, int line) {
        problems.push_back({file, line, "there's no code/frags/" + name + ".html (or .md) to include"});
    }

    // Fragments, resolved depth first.
    FragmentMap* fragments = nullptr;
    std::map<std::string, int> state;   // 1: in progress, 2: done
    std::vector<std::string> chain;

    const CompiledTemplate* resolve(const std::string& name, const std::string& file, int line) {
        auto it = fragments->find(name);
        if (it == fragments->end()) {
            missing(name, file, line);
            return nullptr;
        }
        int& s = state[name];
        if (s == 2) return &it->second;
        if (s == 1) {
            std::string path;
            auto start = std::find(chain.begin(), chain.end(), name);
            for (auto c = start; c != chain.end(); ++c) path += *c + " → ";
            problems.push_back({file, line, "include cycle: " + path + name});
            return nullptr;
        }
        s = 1;
        chain.push_back(name);
        splice(it->second, [this](const std::string& n, const std::string& f, int l) { return resolve(n, f, l); });
        chain.pop_back();
        state[name] = 2;
        return &it->second;
    }
};

std::vector<TemplateProblem> resolveIncludes(FragmentMap& fragments) {
    IncludeResolver r;
    r.fragments = &fragments;
    for (auto& entry : fragments) r.resolve(entry.first, entry.second.source().str(), 0);
    return std::move(r.problems);
}

std::vector<TemplateProblem> resolveIncludes(CompiledTemplate& page, const FragmentMap& fragments) {
    IncludeResolver r;
    r.splice(page, [&](const std::string& name, const std::string& file, int line) -> const CompiledTemplate* {
        auto it = fragments.find(name);
        if (it == fragments.end()) {
            r.missing(name, file, line);
            return nullptr;
        }
        return &it->second;
    });
    return std::move(r.problems);
}

}  // namespace fourfold
