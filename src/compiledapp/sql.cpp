#include "compiledapp/sql.h"

#include <cctype>

namespace fourfold {

namespace {

constexpr size_t kMaxKeys = 1000;
constexpr size_t kMaxPlaceholderBytes = 2000;   // {{key: and its names}}

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80);
}

// Both databases allow '$' inside an identifier (not first).
bool isIdentChar(char c) {
    return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '$';
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

bool isNameKey(const std::string& key) {
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
    for (char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

const char* const kInsideQuotes = "placeholders can't go inside quotes; write {{name}} without them";

class Scanner {
public:
    Scanner(std::string_view s, SqlDialect dialect, std::vector<SqlProblem>& problems)
        : s_(s), dialect_(dialect), problems_(problems) {}

    PreparedSql run() {
        while (i_ < s_.size()) step();
        if (content_) out_.statements++;
        if (!out_.slots.empty()) {
            out_.pieces.push_back(std::move(out_.sql));
            out_.sql.clear();
        }
        return std::move(out_);
    }

private:
    void problem(int line, std::string message) { problems_.push_back({line, std::move(message)}); }

    char at(size_t k) const { return k < s_.size() ? s_[k] : '\0'; }

    // Copies s_[from, to) to the output, counting lines.
    void copy(size_t from, size_t to) {
        for (size_t k = from; k < to; k++) {
            if (s_[k] == '\n') line_++;
        }
        out_.sql.append(s_.data() + from, to - from);
    }

    // Quoted text s_[from, to): a {{ in it is a mistake.
    void checkQuoted(size_t from, size_t to) {
        int line = line_;
        for (size_t k = from; k < to; k++) {
            if (s_[k] == '\n') line++;
            if (s_[k] == '{' && k + 1 < to && s_[k + 1] == '{') {
                problem(line, kInsideQuotes);
                return;
            }
        }
    }

    // A quoted string or identifier starting at i_ (its opening character
    // at `open`): `close` ends it, and doubling `close` escapes it;
    // `backslash` makes \ escape the next character (Postgres's E'...').
    void quoted(size_t open, char close, bool backslash, const char* what) {
        size_t k = open + 1;
        while (k < s_.size()) {
            char c = s_[k];
            if (backslash && c == '\\') {
                k += 2;
                continue;
            }
            if (c == close) {
                if (at(k + 1) == close) {
                    k += 2;
                    continue;
                }
                break;
            }
            k++;
        }
        if (k >= s_.size()) {
            problem(line_, std::string(what) + " isn't closed");
            copy(i_, s_.size());
            i_ = s_.size();
            return;
        }
        checkQuoted(open + 1, k);
        copy(i_, k + 1);
        i_ = k + 1;
        content_ = true;
    }

    void step() {
        char c = s_[i_];
        char next = at(i_ + 1);

        if (c == '-' && next == '-') {   // a comment, to the end of the line
            size_t end = s_.find('\n', i_);
            if (end == std::string_view::npos) end = s_.size();
            copy(i_, end);
            i_ = end;
            return;
        }
        if (c == '/' && next == '*') {   // a block comment; Postgres's nest
            size_t k = i_ + 2;
            int depth = 1;
            while (k < s_.size() && depth > 0) {
                if (s_[k] == '*' && at(k + 1) == '/') {
                    depth--;
                    k += 2;
                } else if (dialect_ == SqlDialect::Postgres && s_[k] == '/' && at(k + 1) == '*') {
                    depth++;
                    k += 2;
                } else {
                    k++;
                }
            }
            if (depth > 0) problem(line_, "a /* comment isn't closed");
            copy(i_, k);
            i_ = k;
            return;
        }
        if (c == '\'') return quoted(i_, '\'', false, "a string ('...')");
        if (c == '"') return quoted(i_, '"', false, "a quoted name (\"...\")");
        if (dialect_ == SqlDialect::Sqlite && c == '`') return quoted(i_, '`', false, "a quoted name (`...`)");
        if (dialect_ == SqlDialect::Sqlite && c == '[') {   // SQLite's [quoted name]
            size_t end = s_.find(']', i_ + 1);
            if (end == std::string_view::npos) {
                problem(line_, "a quoted name ([...]) isn't closed");
                copy(i_, s_.size());
                i_ = s_.size();
                return;
            }
            checkQuoted(i_ + 1, end);
            copy(i_, end + 1);
            i_ = end + 1;
            content_ = true;
            return;
        }

        if (isIdentStart(c)) {   // a word: a keyword, a name, or a string's prefix
            size_t k = i_;
            while (k < s_.size() && isIdentChar(s_[k])) k++;
            bool escapeString =
                dialect_ == SqlDialect::Postgres && k == i_ + 1 && (c == 'E' || c == 'e') && at(k) == '\'';
            copy(i_, k);
            i_ = k;
            content_ = true;
            if (escapeString) quoted(i_, '\'', true, "a string (E'...')");
            return;
        }

        if (c == '$' && dialect_ == SqlDialect::Postgres) {
            if (std::isdigit(static_cast<unsigned char>(next))) {
                problem(line_, "$1-style parameters aren't used here; write placeholders as {{0}} or {{name}}");
                copy(i_, i_ + 1);
                i_++;
                return;
            }
            // A dollar-quoted string: $$...$$ or $tag$...$tag$.
            size_t k = i_ + 1;
            if (k < s_.size() && isIdentStart(s_[k])) {
                while (k < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[k])) || s_[k] == '_' ||
                                         (static_cast<unsigned char>(s_[k]) & 0x80))) {
                    k++;
                }
            }
            if (at(k) == '$') {
                std::string_view tag = s_.substr(i_, k + 1 - i_);
                size_t end = s_.find(tag, k + 1);
                if (end == std::string_view::npos) {
                    problem(line_, "a dollar-quoted string (" + std::string(tag) + "...) isn't closed");
                    copy(i_, s_.size());
                    i_ = s_.size();
                    return;
                }
                checkQuoted(k + 1, end);
                copy(i_, end + tag.size());
                i_ = end + tag.size();
                content_ = true;
                return;
            }
        }
        if (dialect_ == SqlDialect::Sqlite &&
            (c == '?' || ((c == ':' || c == '@' || c == '$') && (isIdentChar(next) && next != '$')))) {
            problem(line_, std::string("SQLite would read ") + (c == '?' ? "?" : std::string(1, c) + "...") +
                               " as a parameter; write placeholders as {{0}} or {{name}}");
            copy(i_, i_ + 1);
            i_++;
            return;
        }

        if (c == '{' && next == '{') return placeholder();

        if (c == ';') {
            if (content_) out_.statements++;
            content_ = false;
        } else if (!isSpace(c)) {
            content_ = true;
        }
        copy(i_, i_ + 1);
        i_++;
    }

    void placeholder() {
        size_t end = s_.find("}}", i_ + 2);
        size_t newline = s_.find('\n', i_ + 2);
        if (end == std::string_view::npos || (newline != std::string_view::npos && newline < end) ||
            end - i_ > kMaxPlaceholderBytes) {
            problem(line_, "{{ starts a placeholder, but there's no }} to end it");
            copy(i_, i_ + 2);
            i_ += 2;
            return;
        }
        std::string inside(s_.substr(i_ + 2, end - i_ - 2));
        std::string key = trim(inside.substr(0, inside.find(':')));
        int line = line_;
        i_ = end + 2;
        content_ = true;
        if (inside.find(':') != std::string::npos) return slot(key, inside.substr(inside.find(':') + 1), line);
        if (!isNameKey(key) && !isSqlIndexKey(key)) {
            problem(line_, "{{" + key +
                               "}} isn't a placeholder: write a name (letters, digits and _, not starting with a "
                               "digit) or an index (0, 1, 2...)");
            return;
        }
        for (const SqlSlot& slot : out_.slots) {
            if (slot.key == key) {
                problem(line_, "{{" + key + "}} is a name slot elsewhere in this query; a key is a value or a name, "
                               "not both");
                return;
            }
        }
        size_t n = 0;
        while (n < out_.keys.size() && out_.keys[n] != key) n++;
        if (n == out_.keys.size()) {
            if (out_.keys.size() >= kMaxKeys) {
                problem(line_, "too many placeholders (at most " + std::to_string(kMaxKeys) + ")");
                return;
            }
            out_.keys.push_back(key);
        }
        out_.sql += (dialect_ == SqlDialect::Sqlite ? "?" : "$") + std::to_string(n + 1);
        // $1abc would read as one (bad) token.
        if (i_ < s_.size() && isIdentChar(s_[i_])) out_.sql += ' ';
    }

    static std::string trim(const std::string& text) {
        size_t a = 0, b = text.size();
        while (a < b && isSpace(text[a])) a++;
        while (b > a && isSpace(text[b - 1])) b--;
        return text.substr(a, b - a);
    }

    // A plain name, or two joined by a dot: title, notes.title.
    static bool isPlainName(const std::string& name) {
        size_t dot = name.find('.');
        if (dot == std::string::npos) return isNameKey(name);
        return isNameKey(name.substr(0, dot)) && isNameKey(name.substr(dot + 1));
    }

    // {{key: a, b, c}}: an identifier slot.
    void slot(const std::string& key, const std::string& list, int line) {
        if (!isNameKey(key) && !isSqlIndexKey(key)) {
            problem(line, "{{" + key + ": ...}} isn't a name slot: write a key (a name, or an index), a colon, and "
                          "the names it may be, like {{sort: title, created}}");
            return;
        }
        SqlSlot slot;
        slot.key = key;
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string name = trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!isPlainName(name)) {
                problem(line, "{{" + key + ": ...}}: \"" + name +
                                  "\" isn't a plain name (letters, digits and _, not starting with a digit; or two "
                                  "joined by a dot)");
                return;
            }
            slot.options.push_back(name);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        for (const std::string& k : out_.keys) {
            if (k == key) {
                problem(line, "{{" + key + "}} is a value elsewhere in this query; a key is a value or a name, not "
                              "both");
                return;
            }
        }
        for (const SqlSlot& other : out_.slots) {
            if (other.key == key && other.options != slot.options) {
                problem(line, "{{" + key + ": ...}} appears twice with different names; give it the same list each "
                              "time");
                return;
            }
        }
        out_.pieces.push_back(std::move(out_.sql));
        out_.sql.clear();
        out_.slots.push_back(std::move(slot));
        // A name glued to what follows would read as one word.
        if (i_ < s_.size() && isIdentChar(s_[i_])) out_.sql += ' ';
    }

    std::string_view s_;
    SqlDialect dialect_;
    std::vector<SqlProblem>& problems_;
    PreparedSql out_;
    size_t i_ = 0;
    int line_ = 1;
    bool content_ = false;   // the statement so far has something besides space and comments
};

}  // namespace

const char* sqlDialectName(SqlDialect dialect) {
    return dialect == SqlDialect::Sqlite ? "SQLite" : "Postgres";
}

const char* sqlDialectSuffix(SqlDialect dialect) {
    return dialect == SqlDialect::Sqlite ? "sqlite" : "pg";
}

bool PreparedSql::allIndexes() const {
    for (const std::string& k : allKeys()) {
        if (!isSqlIndexKey(k)) return false;
    }
    return true;
}

std::vector<std::string> PreparedSql::allKeys() const {
    std::vector<std::string> out = keys;
    for (const SqlSlot& slot : slots) out.push_back(slot.key);
    return out;
}

bool isSqlIndexKey(const std::string& key) {
    if (key.empty() || key.size() > 9) return false;
    for (char c : key) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return key == "0" || key[0] != '0';
}

PreparedSql prepareSql(std::string_view source, SqlDialect dialect, std::vector<SqlProblem>& problems) {
    return Scanner(source, dialect, problems).run();
}

Result<std::vector<Datum>> bindValues(const PreparedSql& sql, const Datum& values) {
    std::vector<Datum> out;
    out.reserve(sql.keys.size());
    if (values.kind() == Datum::Kind::List) {
        const Datum::ListItems& items = values.asList();
        for (const std::string& key : sql.keys) {
            if (!isSqlIndexKey(key)) {
                return makeError(ErrorCode::Invalid, "this query has a named placeholder, {{" + key +
                                                         "}}, so its values must be a map, not a list");
            }
            size_t index = std::stoul(key);
            if (index >= items.size()) {
                return makeError(ErrorCode::Invalid, "no value for {{" + key + "}}: the list has " +
                                                         std::to_string(items.size()) +
                                                         (items.size() == 1 ? " item" : " items"));
            }
            out.push_back(items[index]);
        }
        return out;
    }
    if (values.kind() == Datum::Kind::Map) {
        const Datum::MapItems& items = values.asMap();
        for (const std::string& key : sql.keys) {
            auto it = items.find(key);
            if (it == items.end()) return makeError(ErrorCode::Invalid, "no value for {{" + key + "}}");
            out.push_back(it->second);
        }
        return out;
    }
    if (values.isNull() && sql.keys.empty()) return out;
    if (values.isNull()) {
        return makeError(ErrorCode::Invalid, "this query has placeholders ({{" + sql.keys[0] +
                                                 "}}...), so it needs values: a list or a map");
    }
    return makeError(ErrorCode::Invalid, "the values must be a list or a map");
}

namespace {

// The value for `key` in `values` (a list or a map), or nullptr.
const Datum* valueFor(const Datum& values, const std::string& key) {
    if (values.kind() == Datum::Kind::Map) {
        auto it = values.asMap().find(key);
        return it == values.asMap().end() ? nullptr : &it->second;
    }
    if (values.kind() == Datum::Kind::List && isSqlIndexKey(key)) {
        size_t index = std::stoul(key);
        return index < values.asList().size() ? &values.asList()[index] : nullptr;
    }
    return nullptr;
}

bool sameIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string choices(const std::vector<std::string>& options) {
    std::string s;
    for (size_t i = 0; i < options.size(); i++) {
        if (i > 0) s += i + 1 == options.size() ? " or " : ", ";
        s += options[i];
    }
    return s;
}

}  // namespace

Result<BoundQuery> bindQuery(const PreparedSql& sql, const Datum& values) {
    BoundQuery out;
    if (!sql.slots.empty() && values.kind() == Datum::Kind::List) {
        for (const SqlSlot& slot : sql.slots) {
            if (!isSqlIndexKey(slot.key)) {
                return makeError(ErrorCode::Invalid, "this query has a named slot, {{" + slot.key +
                                                         ": ...}}, so its values must be a map, not a list");
            }
        }
    }
    auto params = bindValues(sql, values);
    if (!params) return params.error();
    out.params = std::move(params).value();
    if (sql.slots.empty()) {
        out.sql = sql.sql;
        return out;
    }
    if (values.kind() != Datum::Kind::Map && values.kind() != Datum::Kind::List) {
        return makeError(ErrorCode::Invalid, "this query has a name slot ({{" + sql.slots[0].key +
                                                 ": ...}}), so it needs values: a map or a list");
    }
    out.sql = sql.pieces[0];
    for (size_t i = 0; i < sql.slots.size(); i++) {
        const SqlSlot& slot = sql.slots[i];
        const Datum* v = valueFor(values, slot.key);
        if (!v) return makeError(ErrorCode::Invalid, "no value for {{" + slot.key + "}}");
        const std::string* chosen = nullptr;
        if (v->kind() == Datum::Kind::String) {
            for (const std::string& option : slot.options) {
                if (sameIgnoringCase(option, v->asString())) chosen = &option;
            }
        }
        if (!chosen) {
            return makeError(ErrorCode::Invalid, "{{" + slot.key + "}} must be " + choices(slot.options) +
                                                     (v->kind() == Datum::Kind::String ? ", not \"" + v->asString() + "\""
                                                                                       : ""));
        }
        out.sql += *chosen;
        out.sql += sql.pieces[i + 1];
    }
    return out;
}

}  // namespace fourfold
