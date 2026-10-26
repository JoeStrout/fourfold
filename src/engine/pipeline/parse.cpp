#include <cctype>

#include "base/utf8.h"
#include "pipeline/stages.h"

namespace fourfold {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Replaces each byte that isn't part of valid UTF-8 with U+FFFD.
std::string validUtf8(std::string s) {
    if (isValidUtf8(s)) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        // Try the longest valid sequence starting here (at most 4 bytes).
        size_t len = 0;
        for (size_t n = 1; n <= 4 && i + n <= s.size(); n++) {
            if (isValidUtf8(std::string_view(s).substr(i, n))) {
                len = n;
                break;
            }
        }
        if (len) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
        }
    }
    return out;
}

std::string decodeComponent(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return validUtf8(std::move(out));
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

RequestFacts::Pairs parseUrlEncoded(std::string_view text) {
    RequestFacts::Pairs pairs;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('&', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view part = text.substr(start, end - start);
        if (!part.empty()) {
            size_t eq = part.find('=');
            std::string_view name = part.substr(0, eq);
            std::string_view value = eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1);
            pairs.emplace_back(decodeComponent(name), decodeComponent(value));
        }
        start = end + 1;
    }
    return pairs;
}

namespace {

// A Cookie header without the engine's own cookies (ff_session, ff_csrf, and
// their __Host- forms): app code has no use for them, and a handler that
// logs its headers mustn't leak a session ID.
std::string withoutEngineCookies(const std::string& header) {
    std::string out;
    size_t start = 0;
    while (start <= header.size()) {
        size_t semi = header.find(';', start);
        if (semi == std::string::npos) semi = header.size();
        std::string pair = header.substr(start, semi - start);
        size_t first = pair.find_first_not_of(" \t");
        std::string name = first == std::string::npos ? "" : pair.substr(first);
        bool engine = name.compare(0, 3, "ff_") == 0 || name.compare(0, 10, "__Host-ff_") == 0;
        if (!engine && first != std::string::npos) {
            if (!out.empty()) out += "; ";
            out += pair.substr(first);
        }
        start = semi + 1;
    }
    return out;
}

}  // namespace

RequestFacts requestFacts(const HttpRequest& request, const AdmittedRequest& admitted,
                          std::optional<std::string>* csrfToken) {
    RequestFacts facts;
    facts.method = admitted.method;
    facts.path = admitted.path.path;
    if (admitted.path.trailingSlash && facts.path != "/") facts.path += "/";
    facts.query = parseUrlEncoded(admitted.query);

    for (const Headers::Field& f : request.headers) {
        std::string name = lower(f.name);
        std::string value = f.value;
        if (name == "x-csrf-token") {
            if (csrfToken && !*csrfToken) *csrfToken = value;
            continue;
        }
        bool cookie = name == "cookie";
        if (cookie) {
            // Field by field, before merging, so no engine cookie can hide
            // inside another's value.
            value = withoutEngineCookies(value);
            if (value.empty()) continue;
        }
        bool merged = false;
        for (auto& [existing, v] : facts.headers) {
            if (existing == name) {
                v += (cookie ? "; " : ", ") + value;   // Cookie fields join with "; " (RFC 6265 §5.4)
                merged = true;
                break;
            }
        }
        if (!merged) facts.headers.emplace_back(name, value);
    }
    for (auto& [name, value] : facts.headers) value = validUtf8(std::move(value));

    if (admitted.method == "POST") {
        const std::string* type = request.headers.get("Content-Type");
        std::string mediaType = type ? lower(type->substr(0, type->find(';'))) : "";
        while (!mediaType.empty() && mediaType.back() == ' ') mediaType.pop_back();
        if (mediaType == "application/x-www-form-urlencoded") facts.form = parseUrlEncoded(request.body);
    }
    for (auto it = facts.form.begin(); it != facts.form.end();) {
        if (it->first == "_csrf") {
            if (csrfToken && !*csrfToken) *csrfToken = it->second;
            it = facts.form.erase(it);
        } else {
            ++it;
        }
    }
    return facts;
}

}  // namespace fourfold
