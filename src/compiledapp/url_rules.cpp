#include "compiledapp/url_rules.h"

#include <algorithm>

namespace fourfold {

namespace {

bool isNameStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool isNameChar(char c) { return isNameStart(c) || (c >= '0' && c <= '9'); }

bool isName(std::string_view s) {
    if (s.empty() || !isNameStart(s[0])) return false;
    return std::all_of(s.begin(), s.end(), isNameChar);
}

// First URL segments the engine keeps for itself, as in the routes pass.
bool isReservedSegment(std::string_view s) {
    return s == "__auth" || s == "__fourfold" || s == "__dev" || s == "live";
}

// Characters a target may not hold as literal text: what isn't valid in a
// URL, or would need encoding to be.
bool isBadTargetChar(char ch) {
    unsigned char c = static_cast<unsigned char>(ch);
    return c <= 0x20 || c >= 0x7F || ch == '"' || ch == '<' || ch == '>' || ch == '\\' || ch == '^' || ch == '`' ||
           ch == '|';
}

void collapseSlashes(std::string& s, size_t from, size_t to) {
    std::string out = s.substr(0, from);
    for (size_t i = from; i < to; i++) {
        if (s[i] == '/' && !out.empty() && out.back() == '/' && i > from) continue;
        out += s[i];
    }
    out += s.substr(to);
    s = std::move(out);
}

}  // namespace

std::string encodeQueryComponent(std::string_view value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

// --- UrlPattern ---

std::optional<UrlPattern> UrlPattern::parse(const std::string& text, std::string& error) {
    if (text.empty() || text[0] != '/') {
        error = "a pattern is a URL path, starting with /";
        return std::nullopt;
    }
    if (text.find_first_of("?#") != std::string::npos) {
        error = "a pattern is a path only; it can't match a query (?) or fragment (#)";
        return std::nullopt;
    }
    UrlPattern p;
    p.text_ = text;
    p.trailingSlash_ = text.size() > 1 && text.back() == '/';
    std::string body = text.substr(1, text.size() - 1 - (p.trailingSlash_ ? 1 : 0));
    if (body.empty()) {
        if (p.trailingSlash_) {
            error = "write the root as \"/\"";
            return std::nullopt;
        }
        return p;   // "/"
    }
    size_t start = 0;
    while (start <= body.size()) {
        size_t slash = body.find('/', start);
        if (slash == std::string::npos) slash = body.size();
        std::string seg = body.substr(start, slash - start);
        start = slash + 1;
        if (seg.empty()) {
            error = "a pattern can't have an empty segment (//)";
            return std::nullopt;
        }
        Segment s;
        if (seg.front() == '{') {
            if (seg.back() != '}') {
                error = "a placeholder is a whole segment, like /{code}";
                return std::nullopt;
            }
            std::string name = seg.substr(1, seg.size() - 2);
            if (!name.empty() && name.back() == '*') {
                s.rest = true;
                name.pop_back();
            }
            if (!isName(name)) {
                error = "{" + name + "} isn't a placeholder name (a letter or _, then letters, digits or _)";
                return std::nullopt;
            }
            for (const Segment& other : p.segments_) {
                if (other.placeholder && other.text == name) {
                    error = "{" + name + "} appears twice";
                    return std::nullopt;
                }
            }
            s.text = std::move(name);
            s.placeholder = true;
        } else {
            if (seg.find_first_of("{}") != std::string::npos) {
                error = "a placeholder is a whole segment, like /{code}; \"" + seg + "\" isn't one";
                return std::nullopt;
            }
            auto canonical = canonicalUrlPath("/" + seg);
            if (!canonical || canonical.value().path != "/" + seg) {
                error = seg.find('%') != std::string::npos ? "write \"" + seg + "\" decoded, without % escapes"
                                                           : "\"" + seg + "\" isn't a valid URL segment";
                return std::nullopt;
            }
            if (p.segments_.empty() && isReservedSegment(seg)) {
                error = "/" + seg + "/ is reserved for Fourfold's own use";
                return std::nullopt;
            }
            s.text = std::move(seg);
        }
        if (!p.segments_.empty() && p.segments_.back().rest) {
            error = "{" + p.segments_.back().text + "*} matches the rest of the path, so it must come last";
            return std::nullopt;
        }
        p.segments_.push_back(std::move(s));
    }
    if (p.trailingSlash_ && p.segments_.back().rest) {
        error = "{" + p.segments_.back().text + "*} already matches any trailing slash; drop the last /";
        return std::nullopt;
    }
    return p;
}

bool UrlPattern::hasPlaceholders() const {
    return std::any_of(segments_.begin(), segments_.end(), [](const Segment& s) { return s.placeholder; });
}

std::vector<std::string> UrlPattern::names() const {
    std::vector<std::string> out;
    for (const Segment& s : segments_) {
        if (s.placeholder) out.push_back(s.text);
    }
    return out;
}

std::string UrlPattern::literalPrefix() const {
    std::string out;
    for (const Segment& s : segments_) {
        if (s.placeholder) break;
        out += "/" + s.text;
    }
    return out.empty() ? "/" : out;
}

bool UrlPattern::match(const UrlPath& path, UrlValues& values) const {
    values.clear();
    std::vector<std::string_view> parts;
    std::string_view rest(path.path);
    if (rest.size() > 1) {
        rest.remove_prefix(1);
        while (true) {
            size_t slash = rest.find('/');
            parts.push_back(rest.substr(0, slash));
            if (slash == std::string_view::npos) break;
            rest.remove_prefix(slash + 1);
        }
    }
    for (size_t i = 0; i < segments_.size(); i++) {
        const Segment& s = segments_[i];
        if (s.rest) {
            std::string value;
            for (size_t j = i; j < parts.size(); j++) {
                if (j > i) value += '/';
                value += parts[j];
            }
            if (path.trailingSlash && i < parts.size()) value += '/';
            values.emplace_back(s.text, std::move(value));
            return true;
        }
        if (i >= parts.size()) return false;
        if (s.placeholder) {
            values.emplace_back(s.text, std::string(parts[i]));
        } else if (s.text != parts[i]) {
            return false;
        }
    }
    return parts.size() == segments_.size() && path.trailingSlash == trailingSlash_;
}

bool UrlPattern::coveredBy(const UrlPattern& other) const {
    for (size_t j = 0; j < other.segments_.size(); j++) {
        const Segment& o = other.segments_[j];
        if (o.rest) return true;
        if (j >= segments_.size()) return false;
        const Segment& s = segments_[j];
        if (s.rest) return false;   // matches more lengths than `other` does
        if (!o.placeholder && (s.placeholder || s.text != o.text)) return false;
    }
    return segments_.size() == other.segments_.size() && trailingSlash_ == other.trailingSlash_;
}

// --- UrlTarget ---

std::optional<UrlTarget> UrlTarget::parse(const std::string& text, const UrlPattern& pattern, std::string& error) {
    UrlTarget t;
    t.text_ = text;
    size_t pos = 0;
    for (const char* scheme : {"http://", "https://"}) {
        size_t n = std::char_traits<char>::length(scheme);
        if (text.compare(0, n, scheme) == 0) {
            t.kind_ = Kind::Absolute;
            size_t hostEnd = text.find_first_of("/?#", n);
            if (hostEnd == std::string::npos) hostEnd = text.size();
            std::string host = text.substr(n, hostEnd - n);
            if (host.empty()) {
                error = "the URL has no host";
                return std::nullopt;
            }
            if (host.find_first_of("{}") != std::string::npos) {
                error = "a placeholder can't choose the host; put placeholders in the path or query";
                return std::nullopt;
            }
            for (char c : host) {
                if (isBadTargetChar(c) || c == '@') {
                    error = "\"" + host + "\" isn't a host name";
                    return std::nullopt;
                }
            }
            pos = hostEnd;
            t.pathStart_ = hostEnd;
            t.parts_.push_back({text.substr(0, hostEnd)});
            break;
        }
    }
    if (t.kind_ == Kind::Local) {
        if (text.empty() || text[0] != '/' || (text.size() > 1 && text[1] == '/')) {
            error = "a target is a path starting with / (on this site) or a full http:// or https:// URL";
            return std::nullopt;
        }
    }
    std::vector<std::string> names = pattern.names();
    bool inQuery = false;
    std::string literal;
    auto flush = [&] {
        if (!literal.empty()) t.parts_.push_back({std::move(literal), false, inQuery});
        literal.clear();
    };
    while (pos < text.size()) {
        char c = text[pos];
        if (c == '{') {
            size_t close = text.find('}', pos);
            if (close == std::string::npos) {
                error = "a { with no }";
                return std::nullopt;
            }
            std::string name = text.substr(pos + 1, close - pos - 1);
            if (!name.empty() && name.back() == '*') {
                error = "write {" + name.substr(0, name.size() - 1) + "} in the target (the * goes in the pattern)";
                return std::nullopt;
            }
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                error = "{" + name + "} isn't a placeholder in the pattern";
                return std::nullopt;
            }
            flush();
            t.parts_.push_back({name, true, inQuery});
            pos = close + 1;
            continue;
        }
        if (c == '}') {
            error = "a } with no {";
            return std::nullopt;
        }
        if (isBadTargetChar(c)) {
            error = c == ' ' ? "a target can't contain a space (write %20)"
                             : "a target can't contain that character as is; percent-encode it";
            return std::nullopt;
        }
        if ((c == '?' || c == '#') && !inQuery) {
            flush();
            inQuery = true;
        }
        literal += c;
        pos++;
    }
    flush();
    return t;
}

bool UrlTarget::hasPathPlaceholders() const {
    return std::any_of(parts_.begin(), parts_.end(), [](const Part& p) { return p.placeholder && !p.inQuery; });
}

std::optional<UrlPath> UrlTarget::fixedPath() const {
    if (kind_ != Kind::Local || hasPathPlaceholders()) return std::nullopt;
    auto path = canonicalUrlPath(text_);
    if (!path) return std::nullopt;
    return path.value();
}

std::string UrlTarget::fill(const UrlValues& values) const {
    std::string out;
    size_t pathEnd = std::string::npos;
    for (const Part& p : parts_) {
        if (p.inQuery && pathEnd == std::string::npos) pathEnd = out.size();
        if (!p.placeholder) {
            out += p.text;
            continue;
        }
        for (const auto& [name, value] : values) {
            if (name != p.text) continue;
            out += p.inQuery ? encodeQueryComponent(value) : encodeUrlPath(value);
            break;
        }
    }
    if (pathEnd == std::string::npos) pathEnd = out.size();
    collapseSlashes(out, kind_ == Kind::Absolute ? pathStart_ : 0, pathEnd);
    return out;
}

}  // namespace fourfold
