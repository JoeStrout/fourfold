#include "js_lexer.h"

#include <algorithm>

namespace fourfold {

namespace {

constexpr size_t kMaxReadings = 16;

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Words after which '/' starts a regular expression.
bool isRegexKeyword(const std::string& word) {
    static const char* const kWords[] = {"return", "typeof", "instanceof", "in",    "of",    "new",  "delete",
                                         "void",   "throw",  "case",       "do",    "else",  "yield", "await"};
    for (const char* w : kWords) {
        if (word == w) return true;
    }
    return false;
}

}  // namespace

bool JsLexer::State::operator==(const State& o) const {
    return mode == o.mode && escape == o.escape && star == o.star && dollar == o.dollar &&
           slashIsRegex == o.slashIsRegex && inWord == o.inWord && last == o.last && beforeLast == o.beforeLast &&
           word == o.word && braces == o.braces;
}

JsLexer::JsLexer() : states_{State{}} {}

void JsLexer::code(State& s, char c, std::vector<State>& out) {
    if (isWordChar(c)) {
        if (!s.inWord) s.word.clear();
        s.inWord = true;
        if (s.word.size() < 16) s.word += c;
        s.beforeLast = s.last;
        s.last = 'a';
        out.push_back(s);
        return;
    }
    s.inWord = false;
    if (isSpace(c)) {
        out.push_back(s);
        return;
    }
    switch (c) {
        case '"': s.mode = Mode::DoubleQuote; break;
        case '\'': s.mode = Mode::SingleQuote; break;
        case '`': s.mode = Mode::Template; break;
        case '{':
            if (!s.braces.empty()) s.braces.back()++;
            break;
        case '}':
            if (!s.braces.empty()) {
                if (s.braces.back() == 0) {
                    s.braces.pop_back();
                    s.mode = Mode::Template;   // back inside the template literal
                    out.push_back(s);
                    return;
                }
                s.braces.back()--;
            }
            break;
        case '/': {
            // What does a '/' here start? Decided by what came before.
            bool regex = true, division = false;
            if (s.last == 'a') {
                regex = isRegexKeyword(s.word);
                division = !regex;
            } else if (s.last == ']') {
                regex = false;
                division = true;
            } else if (s.last == ')' || s.last == '}') {
                division = true;   // and regex: both readings
            } else if ((s.last == '+' || s.last == '-') && s.beforeLast == s.last) {
                regex = false;   // `a++ / b`; a regex after ++ would be meaningless
                division = true;
            }
            s.mode = Mode::Slash;
            if (regex) {
                State r = s;
                r.slashIsRegex = true;
                out.push_back(r);
            }
            if (division) {
                s.slashIsRegex = false;
                out.push_back(s);
            }
            return;
        }
        default: break;
    }
    if (s.mode == Mode::Code) {
        s.beforeLast = s.last;
        s.last = c;
    }
    out.push_back(s);
}

void JsLexer::step(State s, char c, std::vector<State>& out) {
    switch (s.mode) {
        case Mode::Code:
            code(s, c, out);
            return;

        case Mode::Slash:
            if (c == '/') {
                s.mode = Mode::LineComment;
                out.push_back(s);
            } else if (c == '*') {
                s.mode = Mode::BlockComment;
                s.star = false;
                out.push_back(s);
            } else if (s.slashIsRegex) {
                s.mode = Mode::Regex;
                step(s, c, out);
            } else {
                s.mode = Mode::Code;
                s.beforeLast = s.last;
                s.last = '/';
                code(s, c, out);
            }
            return;

        case Mode::DoubleQuote:
        case Mode::SingleQuote: {
            char quote = s.mode == Mode::DoubleQuote ? '"' : '\'';
            if (s.escape) {
                s.escape = false;
            } else if (c == '\\') {
                s.escape = true;
            } else if (c == '\n') {
                return;   // a string can't span lines: this reading is wrong
            } else if (c == quote) {
                s.mode = Mode::Code;
                s.beforeLast = s.last;
                s.last = 'a';
                s.word.clear();
            }
            out.push_back(s);
            return;
        }

        case Mode::Template:
            if (s.escape) {
                s.escape = false;
            } else if (c == '\\') {
                s.escape = true;
            } else if (s.dollar && c == '{') {
                s.braces.push_back(0);
                s.mode = Mode::Code;
                s.last = '{';
                s.beforeLast = 0;
            } else if (c == '`') {
                s.mode = Mode::Code;
                s.beforeLast = s.last;
                s.last = 'a';
                s.word.clear();
            }
            s.dollar = !s.escape && c == '$';
            out.push_back(s);
            return;

        case Mode::Regex:
        case Mode::RegexClass:
            if (c == '\n') return;   // nor can a regular expression
            if (s.escape) {
                s.escape = false;
            } else if (c == '\\') {
                s.escape = true;
            } else if (s.mode == Mode::RegexClass) {
                if (c == ']') s.mode = Mode::Regex;
            } else if (c == '[') {
                s.mode = Mode::RegexClass;
            } else if (c == '/') {
                s.mode = Mode::Code;   // its flags read as a word, so a '/' after is a division
                s.beforeLast = s.last;
                s.last = 'a';
                s.word.clear();
                s.inWord = false;
            }
            out.push_back(s);
            return;

        case Mode::LineComment:
            if (c == '\n') s.mode = Mode::Code;
            out.push_back(s);
            return;

        case Mode::BlockComment:
            if (s.star && c == '/') {
                s.mode = Mode::Code;
                s.star = false;
            } else {
                s.star = c == '*';
            }
            out.push_back(s);
            return;
    }
}

void JsLexer::feed(char c) {
    if (lost_) return;
    std::vector<State> next;
    next.reserve(states_.size() + 1);
    for (State& s : states_) step(std::move(s), c, next);
    if (next.size() > 1) {
        std::vector<State> unique;
        for (State& s : next) {
            if (std::find(unique.begin(), unique.end(), s) == unique.end()) unique.push_back(std::move(s));
        }
        next = std::move(unique);
    }
    states_ = std::move(next);
    if (states_.empty() || states_.size() > kMaxReadings) lost_ = true;
}

bool JsLexer::field(std::string& why) {
    if (lost_) {
        why = "the script before it couldn't be followed (is a string or regular expression left open?)";
        return false;
    }
    bool ok = true;
    for (State& s : states_) {
        bool inCode = s.mode == Mode::Code || (s.mode == Mode::Slash && !s.slashIsRegex);
        if (!inCode && ok) {
            ok = false;
            switch (s.mode) {
                case Mode::DoubleQuote:
                case Mode::SingleQuote: why = "it's inside a JavaScript string"; break;
                case Mode::Template: why = "it's inside a JavaScript template literal"; break;
                case Mode::Slash:
                case Mode::Regex:
                case Mode::RegexClass: why = "it may be inside a regular expression"; break;
                case Mode::LineComment:
                case Mode::BlockComment: why = "it's inside a JavaScript comment"; break;
                case Mode::Code: break;
            }
        } else if (ok && !s.braces.empty()) {
            ok = false;
            why = "it's inside a ${...} in a JavaScript template literal";
        }
        // The field is now a value.
        if (s.mode == Mode::Slash && !s.slashIsRegex) {
            s.beforeLast = '/';
        } else {
            s.beforeLast = s.last;
        }
        s.mode = inCode ? Mode::Code : s.mode;
        s.last = 'a';
        s.word.clear();
        s.inWord = false;
    }
    return ok;
}

}  // namespace fourfold
