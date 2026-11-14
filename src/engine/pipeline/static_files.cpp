// Static files (M11.2): validators and conditional requests, ranges, and
// large files sent from disk a piece at a time.
//
// Every static file has an ETag (its size and modification time) and a
// Last-Modified, so a browser revalidating it (Cache-Control: no-cache, the
// default) gets a 304 with no body. One byte range may be asked for (206, or
// 416 if it's past the end), with If-Range, so an interrupted download
// resumes. Small files are read whole; larger ones are streamed, so the
// engine's memory doesn't grow with the files it serves.

#include "pipeline/stages.h"

#include <cctype>
#include <cstdio>

namespace fourfold {

namespace {

// Files up to this size are read into the response; larger ones streamed.
constexpr uint64_t kInlineBytes = 256 * 1024;

std::string hex(uint64_t n) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%llx", static_cast<unsigned long long>(n));
    return buffer;
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

// Whether If-None-Match's list has `etag` (weak comparison, RFC 9110
// §13.1.2: a W/ prefix is ignored), or is "*".
bool noneMatchHas(const std::string& header, const std::string& etag) {
    std::string_view rest(header);
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        std::string tag = trim(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        if (tag == "*") return true;
        if (tag.compare(0, 2, "W/") == 0) tag.erase(0, 2);
        if (tag == etag) return true;
    }
    return false;
}

bool parseNumber(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 19) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return true;
}

// A Range header's one byte range, against a file of `size` bytes.
struct ByteRange {
    enum class Kind { None, Satisfiable, Unsatisfiable };
    Kind kind = Kind::None;   // None: no usable Range; serve the whole file
    uint64_t first = 0;
    uint64_t length = 0;
};

ByteRange parseRange(const std::string& header, uint64_t size) {
    ByteRange r;
    std::string h = trim(header);
    if (h.compare(0, 6, "bytes=") != 0) return r;     // another unit: ignored
    std::string spec = trim(std::string_view(h).substr(6));
    if (spec.find(',') != std::string::npos) return r;   // several ranges: the whole file instead (allowed)
    size_t dash = spec.find('-');
    if (dash == std::string::npos) return r;
    std::string a = trim(std::string_view(spec).substr(0, dash));
    std::string b = trim(std::string_view(spec).substr(dash + 1));
    uint64_t first = 0, last = 0;
    if (a.empty()) {
        // "-N": the last N bytes.
        if (!parseNumber(b, last)) return r;
        if (last == 0 || size == 0) {
            r.kind = ByteRange::Kind::Unsatisfiable;
            return r;
        }
        r.kind = ByteRange::Kind::Satisfiable;
        r.length = std::min(last, size);
        r.first = size - r.length;
        return r;
    }
    if (!parseNumber(a, first)) return r;
    if (b.empty()) {
        last = size == 0 ? 0 : size - 1;
    } else if (!parseNumber(b, last) || last < first) {
        return r;   // malformed: ignored
    }
    if (first >= size) {
        r.kind = ByteRange::Kind::Unsatisfiable;
        return r;
    }
    last = std::min(last, size - 1);
    r.kind = ByteRange::Kind::Satisfiable;
    r.first = first;
    r.length = last - first + 1;
    return r;
}

}  // namespace

HttpResponse serveStatic(const RouteEntry& entry, const CompiledApp& app, const CodeTree& code,
                         const HttpRequest* request) {
    // The file was listed when the app was compiled; it may have gone since
    // (the next compile will drop its route).
    auto opened = code.open(entry.source);
    if (!opened) return HttpResponse::text(404, "Not Found\n");
    OpenCodeFile& file = opened.value();
    const StaticFile* info = app.statics.find(entry.source);

    HttpResponse r;
    r.headers.set("Content-Type", info ? info->contentType : "application/octet-stream");
    std::string etag = "\"" + hex(file.size) + "-" + hex(static_cast<uint64_t>(file.modifiedNs)) + "\"";
    int64_t modified = file.modifiedNs / 1000000000;
    std::string lastModified = httpDate(modified);
    r.headers.set("ETag", etag);
    r.headers.set("Last-Modified", lastModified);
    r.headers.set("Accept-Ranges", "bytes");
    if (info && info->maxAgeSeconds) {
        r.headers.set("Cache-Control", "public, max-age=" + std::to_string(*info->maxAgeSeconds));
    }
    // Asks addStandardHeaders for a sandboxed CSP (an added XML type).
    if (info && info->sandbox) r.headers.set("Content-Security-Policy", "sandbox");

    bool get = request && (request->method == "GET" || request->method == "HEAD");
    if (get) {
        // Conditional requests (RFC 9110 §13.2.2): If-None-Match, else
        // If-Modified-Since.
        const std::string* noneMatch = request->headers.get("If-None-Match");
        bool notModified = false;
        if (noneMatch) {
            notModified = noneMatchHas(*noneMatch, etag);
        } else if (const std::string* since = request->headers.get("If-Modified-Since")) {
            auto t = parseHttpDate(trim(*since));
            notModified = t && modified <= *t;
        }
        if (notModified) {
            r.status = 304;
            r.headers.remove("Content-Type");
            r.headers.remove("Accept-Ranges");
            return r;
        }
    }

    uint64_t first = 0, length = file.size;
    const std::string* range = get && request->method == "GET" ? request->headers.get("Range") : nullptr;
    if (range) {
        // If-Range: the range only if the file is still the one the client
        // has part of (a strong ETag match, or the same Last-Modified);
        // otherwise all of it.
        bool current = true;
        if (const std::string* ifRange = request->headers.get("If-Range")) {
            std::string v = trim(*ifRange);
            current = !v.empty() && v.compare(0, 2, "W/") != 0 && (v.front() == '"' ? v == etag : v == lastModified);
        }
        ByteRange br = current ? parseRange(*range, file.size) : ByteRange{};
        if (br.kind == ByteRange::Kind::Unsatisfiable) {
            HttpResponse bad = HttpResponse::text(416, "Range Not Satisfiable\n");
            bad.headers.set("Content-Range", "bytes */" + std::to_string(file.size));
            return bad;
        }
        if (br.kind == ByteRange::Kind::Satisfiable) {
            r.status = 206;
            first = br.first;
            length = br.length;
            r.headers.set("Content-Range", "bytes " + std::to_string(first) + "-" +
                                               std::to_string(first + length - 1) + "/" + std::to_string(file.size));
        }
    }

    auto source = fileBodySource(std::move(file.fd), first, length);
    if (length <= kInlineBytes) {
        if (!readWholeBody(*source, r.body, kInlineBytes)) return HttpResponse::text(404, "Not Found\n");
    } else {
        r.source = std::move(source);
    }
    return r;
}

}  // namespace fourfold
