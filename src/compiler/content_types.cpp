#include "compiler/content_types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fourfold {

namespace {

struct TypeEntry {
    const char* extension;
    const char* contentType;
};

const TypeEntry kTypes[] = {
    // Text
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".map", "application/json"},
    {".webmanifest", "application/manifest+json"},
    {".txt", "text/plain; charset=utf-8"},
    {".csv", "text/csv; charset=utf-8"},
    // Images
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".avif", "image/avif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    // Fonts
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    // Audio and video
    {".mp3", "audio/mpeg"},
    {".ogg", "audio/ogg"},
    {".wav", "audio/wav"},
    {".mp4", "video/mp4"},
    {".webm", "video/webm"},
    // Other
    {".pdf", "application/pdf"},
    {".wasm", "application/wasm"},
    {".zip", "application/zip"},
};

}  // namespace

const char* contentTypeForExtension(std::string_view extension) {
    std::string lower;
    for (char c : extension) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const TypeEntry& t : kTypes) {
        if (lower == t.extension) return t.contentType;
    }
    return nullptr;
}

namespace {

std::string lower(std::string_view s) {
    std::string out;
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$&-^_.+").find(c) != std::string_view::npos;
}

}  // namespace

std::string checkAddedType(std::string_view extension, std::string& contentType, bool& sandbox, std::string& error) {
    sandbox = false;
    std::string ext = lower(extension);
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    if (ext.empty() || ext.size() > 32 ||
        !std::all_of(ext.begin(), ext.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; })) {
        error = "\"" + std::string(extension) + "\" isn't a file extension (letters, digits, - and _)";
        return "";
    }
    ext = "." + ext;
    if (ext == ".html" || ext == ".htm" || ext == ".md" || ext == ".ms") {
        error = ext + " files are pages or code, not static files";
        return "";
    }
    if (const char* builtIn = contentTypeForExtension(ext)) {
        error = ext + " is already served, as " + std::string(builtIn) + " (built-in types can't be changed)";
        return "";
    }

    // type "/" subtype, then any parameters ("; charset=utf-8").
    std::string text = contentType;
    size_t semi = text.find(';');
    std::string essence = lower(text.substr(0, semi));
    while (!essence.empty() && essence.back() == ' ') essence.pop_back();
    size_t slash = essence.find('/');
    bool ok = slash != std::string::npos && slash > 0 && slash + 1 < essence.size() &&
              std::all_of(essence.begin(), essence.end(), [](char c) { return c == '/' || isTokenChar(c); }) &&
              essence.find('/', slash + 1) == std::string::npos;
    std::string params = semi == std::string::npos ? "" : text.substr(semi);
    for (char c : params) ok = ok && c >= 0x20 && c < 0x7F && c != '"' && c != '\\';
    if (!ok) {
        error = "\"" + contentType + "\" isn't a content type like \"application/octet-stream\"";
        return "";
    }
    std::string subtype = essence.substr(slash + 1);
    // Types a browser would run script from, as the site. Pages are
    // templates, which the compiler checks; these would get around that.
    if (subtype.find("html") != std::string::npos || subtype.find("script") != std::string::npos ||
        subtype.find("svg") != std::string::npos || subtype == "x-javascript" || subtype == "jscript" ||
        subtype.find("xsl") != std::string::npos) {
        error = essence + " can run script as the site, so it can't be added";
        return "";
    }
    sandbox = subtype == "xml" || (subtype.size() > 4 && subtype.compare(subtype.size() - 4, 4, "+xml") == 0);
    contentType = essence + params;
    return ext;
}

}  // namespace fourfold
