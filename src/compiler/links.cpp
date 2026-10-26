#include "links.h"

#include "template/escape.h"

namespace fourfold {

namespace {

// Paths the engine serves itself, which aren't routes.
bool isEnginePath(const std::string& path) {
    return path.compare(0, 12, "/__fourfold/") == 0 || path.compare(0, 7, "/__dev/") == 0;
}

}  // namespace

std::optional<LocalUrl> localUrl(const std::optional<std::vector<std::string>>& folder, const std::string& url,
                                 bool& above) {
    above = false;
    if (!urlScheme(url).empty() || url.compare(0, 2, "//") == 0) return std::nullopt;
    size_t cut = url.find_first_of("?#");
    std::string path = url.substr(0, cut);
    LocalUrl out;
    out.rest = cut == std::string::npos ? "" : url.substr(cut);
    if (path.empty()) return std::nullopt;   // "#anchor" or "?q", on the same page
    bool absolute = path[0] == '/';
    if (!absolute && !folder) return std::nullopt;   // relative to a page we don't know

    out.segments = absolute ? std::vector<std::string>{} : *folder;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        std::string seg = path.substr(start, slash - start);
        start = slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (out.segments.empty()) {
                above = true;
                return std::nullopt;
            }
            out.segments.pop_back();
            continue;
        }
        out.segments.push_back(seg);
    }
    out.trailingSlash = path.back() == '/' || (path.size() >= 2 && path.compare(path.size() - 2, 2, "/.") == 0);
    for (const std::string& s : out.segments) out.path += "/" + s;
    if (out.path.empty()) out.path = "/";
    return out;
}

bool routeExists(const CompiledApp& app, const std::string& path) {
    if (isEnginePath(path)) return true;
    auto canonical = canonicalUrlPath(path);
    if (!canonical) return false;
    return app.routes.find(canonical.value().path) != nullptr;
}

std::string localLinkProblem(const CompiledApp& app, const std::optional<std::vector<std::string>>& folder,
                             const std::string& url, std::string* path) {
    bool above = false;
    std::optional<LocalUrl> local = localUrl(folder, url, above);
    if (above) return "it climbs above the site's root";
    if (!local) return "";
    if (path) *path = local->path;
    if (routeExists(app, local->path)) return "";
    // A page linked by its file name works, through a redirect (pages are
    // served without ".html"), but the link may as well go straight there.
    const std::string& p = local->path;
    if (p.size() > 5 && p.compare(p.size() - 5, 5, ".html") == 0) {
        std::string page = p.substr(0, p.size() - 5);
        bool index = page.size() >= 6 && page.compare(page.size() - 6, 6, "/index") == 0;
        if (index) page.resize(page.size() - 5);   // keep the '/'
        if (routeExists(app, page.empty() ? "/" : page)) {
            std::string written = url.substr(0, url.find_first_of("?#"));
            std::string better = written.substr(0, written.size() - 5);
            if (index) better.resize(better.size() - 5);
            if (better.empty()) better = "./";
            return "it works, by redirecting, but pages are served without .html: write \"" + better +
                   "\" for \"" + written + "\"";
        }
    }
    return "there's no such page or file";
}

}  // namespace fourfold
