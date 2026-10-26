#include "template/escape.h"

namespace fourfold {

const char* const kInvalidUrl = "about:invalid#fourfold";

namespace {

const char kHex[] = "0123456789ABCDEF";

void appendPercent(std::string& out, unsigned char c) {
    out += '%';
    out += kHex[c >> 4];
    out += kHex[c & 15];
}

bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Characters a URL may keep as they are: unreserved and reserved (RFC 3986).
bool isUrlChar(unsigned char c) {
    if (isUnreserved(c)) return true;
    switch (c) {
        case ':': case '/': case '?': case '#': case '[': case ']': case '@':
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*':
        case '+': case ',': case ';': case '=':
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string escapeHtml(std::string_view text) {
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

std::string percentEncode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        if (isUnreserved(c)) {
            out += ch;
        } else {
            appendPercent(out, c);
        }
    }
    return out;
}

std::string urlScheme(std::string_view url) {
    size_t colon = url.find(':');
    if (colon == std::string_view::npos) return "";
    size_t pathStart = url.find_first_of("/?#");
    if (pathStart != std::string_view::npos && pathStart < colon) return "";
    std::string scheme;
    for (char c : url.substr(0, colon)) {
        scheme += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return scheme;
}

std::optional<std::string> filterUrl(std::string_view url) {
    // Browsers drop leading spaces and control characters before parsing.
    size_t start = 0;
    while (start < url.size() && static_cast<unsigned char>(url[start]) <= 0x20) start++;
    url = url.substr(start);
    for (char ch : url) {
        auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return std::nullopt;
    }
    std::string scheme = urlScheme(url);
    if (!scheme.empty() && scheme != "http" && scheme != "https" && scheme != "mailto") return std::nullopt;

    std::string out;
    out.reserve(url.size());
    for (size_t i = 0; i < url.size(); i++) {
        auto c = static_cast<unsigned char>(url[i]);
        if (c == '%' && i + 2 < url.size() && isHexDigit(url[i + 1]) && isHexDigit(url[i + 2])) {
            out += '%';   // an existing escape
        } else if (isUrlChar(c)) {
            out += url[i];
        } else {
            appendPercent(out, c);
        }
    }
    return out;
}

}  // namespace fourfold
