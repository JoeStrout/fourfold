#include "partition/paths.h"

#include "base/utf8.h"

namespace fourfold {

namespace {

constexpr size_t kMaxUrlBytes = 4096;

// Why `segment` isn't allowed, or nullptr if it is.
const char* segmentProblem(std::string_view segment, bool allowLeadingDot) {
    if (segment.empty()) return "empty segment";
    if (segment == "." || segment == "..") return "'.' or '..' segment";
    if (!allowLeadingDot && segment[0] == '.') return "segment starts with '.'";
    if (segment.size() > RelPath::kMaxSegmentBytes) return "segment too long";
    for (char ch : segment) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return "control character";
        if (c == '\\') return "backslash";
        if (c == '/') return "encoded '/'";
    }
    if (!isValidUtf8(segment)) return "invalid UTF-8";
    return nullptr;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Result<RelPath> RelPath::parse(std::string_view path) {
    if (path.empty()) return makeError(ErrorCode::Invalid, "empty path");
    if (path.size() > kMaxPathBytes) return makeError(ErrorCode::Invalid, "path too long");
    if (path[0] == '/') return makeError(ErrorCode::Invalid, "path must be relative");
    size_t start = 0;
    for (;;) {
        size_t slash = path.find('/', start);
        std::string_view segment = path.substr(start, slash == std::string_view::npos ? slash : slash - start);
        if (const char* problem = segmentProblem(segment, false)) {
            return makeError(ErrorCode::Invalid, problem);
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return RelPath(std::string(path));
}

std::vector<std::string_view> RelPath::segments() const {
    std::vector<std::string_view> result;
    std::string_view p = path_;
    size_t start = 0;
    while (start < p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string_view::npos) slash = p.size();
        result.push_back(p.substr(start, slash - start));
        start = slash + 1;
    }
    return result;
}

std::string_view RelPath::name() const {
    size_t slash = path_.rfind('/');
    return std::string_view(path_).substr(slash == std::string::npos ? 0 : slash + 1);
}

std::string_view RelPath::extension() const {
    std::string_view n = name();
    size_t dot = n.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : n.substr(dot);
}

RelPath RelPath::parent() const {
    size_t slash = path_.rfind('/');
    return slash == std::string::npos ? RelPath() : RelPath(path_.substr(0, slash));
}

Result<RelPath> RelPath::child(std::string_view name) const {
    if (name.find('/') != std::string_view::npos) {
        return makeError(ErrorCode::Invalid, "name contains '/'");
    }
    return parse(isRoot() ? std::string(name) : path_ + "/" + std::string(name));
}

bool RelPath::isWithin(const RelPath& folder) const {
    if (folder.isRoot() || *this == folder) return true;
    return path_.size() > folder.path_.size() && path_.compare(0, folder.path_.size(), folder.path_) == 0 &&
           path_[folder.path_.size()] == '/';
}

Result<UrlPath> canonicalUrlPath(std::string_view target) {
    size_t end = target.find_first_of("?#");
    std::string_view raw = target.substr(0, end);
    if (raw.empty() || raw[0] != '/') return makeError(ErrorCode::Invalid, "path must start with '/'");
    if (raw.size() > kMaxUrlBytes) return makeError(ErrorCode::Invalid, "path too long");

    UrlPath result;
    result.trailingSlash = raw.size() > 1 && raw.back() == '/';
    size_t start = 1;
    while (start <= raw.size()) {
        size_t slash = raw.find('/', start);
        if (slash == std::string_view::npos) slash = raw.size();
        std::string_view rawSegment = raw.substr(start, slash - start);
        start = slash + 1;
        if (rawSegment.empty()) continue;   // "//" or a trailing '/'

        // Decode this segment on its own, so an encoded '/' can't make new
        // segments, and "%2e%2e" is caught as "..".
        std::string segment;
        for (size_t i = 0; i < rawSegment.size(); i++) {
            if (rawSegment[i] != '%') {
                segment += rawSegment[i];
                continue;
            }
            int hi = i + 2 < rawSegment.size() ? hexValue(rawSegment[i + 1]) : -1;
            int lo = i + 2 < rawSegment.size() ? hexValue(rawSegment[i + 2]) : -1;
            if (hi < 0 || lo < 0) return makeError(ErrorCode::Invalid, "bad percent escape");
            segment += static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (const char* problem = segmentProblem(segment, true)) {
            return makeError(ErrorCode::Invalid, problem);
        }
        result.path += '/';
        result.path += segment;
    }
    if (result.path.empty()) result.path = "/";
    return result;
}

std::string encodeUrlPath(std::string_view path) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : path) {
        unsigned char c = static_cast<unsigned char>(ch);
        // RFC 3986 unreserved characters, plus '/' and sub-delimiters that are
        // harmless in a path. Everything else (space, '%', '?', '#', '\\',
        // controls, non-ASCII) is encoded.
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    std::string_view("-._~/!$&'()*+,;=:@").find(static_cast<char>(c)) != std::string_view::npos;
        if (safe) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

}  // namespace fourfold
