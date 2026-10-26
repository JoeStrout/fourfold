// Just enough of a JavaScript lexer to tell, at a field inside a <script>,
// whether the field is where an expression can go, rather than inside a
// string, a template literal, a regular expression or a comment.
//
// The one hard part is '/': after ')' or '}' it may start a regular
// expression (`if (x) /re/.test(s)`) or be a division (`(a) / b`), and only
// a full parser knows which. html/template guesses. This lexer doesn't: it
// follows both readings until one becomes impossible (a regular expression
// can't span lines), and a field is allowed only if every reading still
// alive puts it in code.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fourfold {

class JsLexer {
public:
    JsLexer();

    void feed(char c);
    void feed(std::string_view text) {
        for (char c : text) feed(c);
    }

    // A field here. Returns true if it's in code, in every reading; else
    // sets `why`. Either way, the lexer then treats the field as an
    // expression (a value has been written there).
    bool field(std::string& why);

private:
    enum class Mode {
        Code,
        Slash,          // just after a '/' in code
        DoubleQuote,
        SingleQuote,
        Template,       // inside `...`
        Regex,
        RegexClass,     // inside [...] in a regular expression
        LineComment,
        BlockComment,
    };

    struct State {
        Mode mode = Mode::Code;
        bool escape = false;        // the last character was a backslash
        bool star = false;          // BlockComment: the last character was '*'
        bool dollar = false;        // Template: the last character was '$'
        bool slashIsRegex = false;  // Slash: this reading takes the '/' as a regex
        bool inWord = false;        // Code: reading an identifier or number
        char last = 0;              // Code: the last significant character (0 at start; 'a' for a word or value)
        char beforeLast = 0;
        std::string word;           // the last identifier or number, if `last` is 'a'
        std::vector<int> braces;    // for each enclosing ${ in a template literal, the { depth within it

        bool operator==(const State& o) const;
    };

    // Feeds one state; appends it (or its two readings) to `out`, or drops it
    // if the character makes it impossible.
    static void step(State s, char c, std::vector<State>& out);
    static void code(State& s, char c, std::vector<State>& out);

    std::vector<State> states_;
    bool lost_ = false;   // too many readings, or none left
};

}  // namespace fourfold
