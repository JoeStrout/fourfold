#include "base/datum.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <variant>

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
    // The shortest precision that reads back exactly.
    char buf[32];
    for (int precision = 1; precision <= 17; precision++) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, n);
        if (std::strtod(buf, nullptr) == n) break;
    }
    out += buf;
}

void appendString(std::string& out, const std::string& s) {
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

void appendJson(std::string& out, const Datum& d) {
    switch (d.kind()) {
        case Datum::Kind::Null: out += "null"; break;
        case Datum::Kind::Number: appendNumber(out, d.asNumber()); break;
        case Datum::Kind::String: appendString(out, d.asString()); break;
        case Datum::Kind::List: {
            out += '[';
            bool first = true;
            for (const Datum& item : d.asList()) {
                if (!first) out += ',';
                first = false;
                appendJson(out, item);
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
                appendString(out, key);
                out += ':';
                appendJson(out, item);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string toJson(const Datum& value) {
    std::string out;
    appendJson(out, value);
    return out;
}

}  // namespace fourfold
