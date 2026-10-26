#include "base/datum.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <variant>

#include "base/utf8.h"

namespace fourfold {

struct Datum::Node {
    std::variant<double, std::string, ListItems, MapItems> value;
};

Datum Datum::number(double value) {
    return Datum(std::make_shared<const Node>(Node{value}));
}

Datum Datum::string(std::string value) {
    return Datum(std::make_shared<const Node>(Node{std::move(value)}));
}

Datum Datum::list(ListItems items) {
    return Datum(std::make_shared<const Node>(Node{std::move(items)}));
}

Datum Datum::map(MapItems items) {
    return Datum(std::make_shared<const Node>(Node{std::move(items)}));
}

Datum::Kind Datum::kind() const {
    if (!node_) return Kind::Null;
    switch (node_->value.index()) {
        case 0: return Kind::Number;
        case 1: return Kind::String;
        case 2: return Kind::List;
        default: return Kind::Map;
    }
}

double Datum::asNumber() const { return std::get<double>(node_->value); }
const std::string& Datum::asString() const { return std::get<std::string>(node_->value); }
const Datum::ListItems& Datum::asList() const { return std::get<ListItems>(node_->value); }
const Datum::MapItems& Datum::asMap() const { return std::get<MapItems>(node_->value); }

bool Datum::operator==(const Datum& other) const {
    if (node_ == other.node_) return true;
    if (!node_ || !other.node_) return false;
    return node_->value == other.node_->value;
}

namespace {

void appendNumber(std::string& out, double n) {
    if (!std::isfinite(n)) {   // not expected (see datum.h), but never emit invalid JSON
        out += "null";
        return;
    }
    if (n == 0) {   // also turns -0 into 0
        out += "0";
        return;
    }
    char buf[32];
    // Whole numbers as whole numbers ("400", not "4e+02"), up to 2^53, past
    // which doubles skip integers.
    if (n == std::floor(n) && std::fabs(n) <= 9007199254740992.0) {
        std::snprintf(buf, sizeof buf, "%.0f", n);
        out += buf;
        return;
    }
    // Otherwise the shortest precision that reads back exactly.
    for (int precision = 1; precision <= 17; precision++) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, n);
        if (std::strtod(buf, nullptr) == n) break;
    }
    out += buf;
}

void appendString(std::string& out, const std::string& s, bool forScript) {
    static const char* const kHex = "0123456789abcdef";
    out += '"';
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '/': case '`': case '\'':
                if (!forScript) {
                    out += static_cast<char>(c);
                    break;
                }
                [[fallthrough]];
            case '<': case '>': case '&':
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
                break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0xF];
                } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                           (static_cast<unsigned char>(s[i + 2]) == 0xA8 ||
                            static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
                    // U+2028 and U+2029 end a line in JavaScript.
                    out += static_cast<unsigned char>(s[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
                    i += 2;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void appendJson(std::string& out, const Datum& d, bool forScript) {
    switch (d.kind()) {
        case Datum::Kind::Null: out += "null"; break;
        case Datum::Kind::Number: appendNumber(out, d.asNumber()); break;
        case Datum::Kind::String: appendString(out, d.asString(), forScript); break;
        case Datum::Kind::List: {
            out += '[';
            bool first = true;
            for (const Datum& item : d.asList()) {
                if (!first) out += ',';
                first = false;
                appendJson(out, item, forScript);
            }
            out += ']';
            break;
        }
        case Datum::Kind::Map: {
            out += '{';
            bool first = true;
            for (const auto& [key, item] : d.asMap()) {
                if (!first) out += ',';
                first = false;
                appendString(out, key, forScript);
                out += ':';
                appendJson(out, item, forScript);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string toJson(const Datum& value) {
    std::string out;
    appendJson(out, value, false);
    return out;
}

std::string toJsonForScript(const Datum& value) {
    std::string out;
    appendJson(out, value, true);
    return out;
}

namespace {

// A recursive-descent JSON reader. Depth is bounded by the limits, so the
// recursion is too.
class JsonReader {
public:
    JsonReader(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

    Result<Datum> read() {
        std::optional<Datum> value = readValue(0);
        if (value) {
            skipSpace();
            if (pos_ != text_.size()) fail("unexpected text after the value");
        }
        if (!error_.empty()) {
            return makeError(ErrorCode::Invalid, "bad JSON at byte " + std::to_string(errorPos_) + ": " + error_);
        }
        return std::move(*value);
    }

private:
    std::optional<Datum> fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message;
            errorPos_ = pos_;
        }
        return std::nullopt;
    }

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            pos_++;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    std::optional<Datum> readValue(int depth) {
        if (depth > limits_.maxDepth) return fail("nested more than " + std::to_string(limits_.maxDepth) + " deep");
        skipSpace();
        if (pos_ >= text_.size()) return fail("unexpected end");
        char c = text_[pos_];
        if (c == '{') return readMap(depth);
        if (c == '[') return readList(depth);
        if (c == '"') {
            std::optional<std::string> s = readString();
            if (!s) return std::nullopt;
            return Datum::string(std::move(*s));
        }
        if (c == '-' || (c >= '0' && c <= '9')) return readNumber();
        if (literal("null")) return Datum();
        // Fourfold's data has no booleans: MiniScript's true and false are
        // 1 and 0, so they read back as the numbers they were.
        if (literal("true")) return Datum::number(1);
        if (literal("false")) return Datum::number(0);
        return fail("unexpected character");
    }

    std::optional<Datum> readList(int depth) {
        pos_++;   // '['
        Datum::ListItems items;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            pos_++;
            return Datum::list(std::move(items));
        }
        while (true) {
            std::optional<Datum> item = readValue(depth + 1);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end in a list");
            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (text_[pos_] == ']') {
                pos_++;
                return Datum::list(std::move(items));
            }
            return fail("expected ',' or ']'");
        }
    }

    std::optional<Datum> readMap(int depth) {
        pos_++;   // '{'
        Datum::MapItems items;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            pos_++;
            return Datum::map(std::move(items));
        }
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a string key");
            size_t keyPos = pos_;
            std::optional<std::string> key = readString();
            if (!key) return std::nullopt;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            pos_++;
            std::optional<Datum> item = readValue(depth + 1);
            if (!item) return std::nullopt;
            if (!items.emplace(std::move(*key), std::move(*item)).second) {
                pos_ = keyPos;
                return fail("repeated key");
            }
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end in a map");
            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (text_[pos_] == '}') {
                pos_++;
                return Datum::map(std::move(items));
            }
            return fail("expected ',' or '}'");
        }
    }

    std::optional<Datum> readNumber() {
        size_t start = pos_;
        auto digits = [&] {
            size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
            return pos_ > from;
        };
        if (text_[pos_] == '-') pos_++;
        if (pos_ < text_.size() && text_[pos_] == '0') {
            pos_++;
        } else if (!digits()) {
            return fail("bad number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            pos_++;
            if (!digits()) return fail("bad number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
            if (!digits()) return fail("bad number");
        }
        std::string token(text_.substr(start, pos_ - start));
        double n = std::strtod(token.c_str(), nullptr);
        if (!std::isfinite(n)) {
            pos_ = start;
            return fail("number too large");
        }
        return Datum::number(n == 0 ? 0.0 : n);   // no -0, as toJson writes it
    }

    // Four hex digits at pos_, or -1.
    int readHex4() {
        if (pos_ + 4 > text_.size()) return -1;
        int v = 0;
        for (int i = 0; i < 4; i++) {
            char c = text_[pos_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        pos_ += 4;
        return v;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
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

    std::optional<std::string> readString() {
        pos_++;   // '"'
        std::string out;
        size_t runStart = pos_;   // raw bytes not yet copied
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
                return std::nullopt;
            }
            unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                out.append(text_.substr(runStart, pos_ - runStart));
                pos_++;
                if (!isValidUtf8(out)) {
                    fail("invalid UTF-8 in a string");
                    return std::nullopt;
                }
                return out;
            }
            if (c < 0x20) {
                fail("control character in a string");
                return std::nullopt;
            }
            if (c != '\\') {
                pos_++;
                continue;
            }
            out.append(text_.substr(runStart, pos_ - runStart));
            pos_++;
            if (pos_ >= text_.size()) {
                fail("unterminated string");
                return std::nullopt;
            }
            char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    int hi = readHex4();
                    if (hi < 0) {
                        fail("bad \\u escape");
                        return std::nullopt;
                    }
                    uint32_t cp = static_cast<uint32_t>(hi);
                    if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("lone surrogate");
                        return std::nullopt;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        int lo = -1;
                        if (literal("\\u")) lo = readHex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            fail("lone surrogate");
                            return std::nullopt;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    pos_--;
                    fail("bad escape");
                    return std::nullopt;
            }
            runStart = pos_;
        }
    }

    std::string_view text_;
    const JsonLimits& limits_;
    size_t pos_ = 0;
    std::string error_;
    size_t errorPos_ = 0;
};

}  // namespace

Result<Datum> parseJson(std::string_view text, const JsonLimits& limits) {
    if (text.size() > limits.maxBytes) {
        return makeError(ErrorCode::TooLarge, "JSON text is over " + std::to_string(limits.maxBytes) + " bytes");
    }
    return JsonReader(text, limits).read();
}

}  // namespace fourfold
