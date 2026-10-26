// Checking local links against the route table, for HTML templates (the
// links pass) and Markdown pages (the Markdown pass).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"

namespace fourfold {

// A URL as written in a page, resolved to the site path it requests.
struct LocalUrl {
    std::string path;        // "/docs/setup", ".." and "." resolved; "/" for the root
    bool trailingSlash = false;
    std::string rest;        // the "?query#fragment", if any
    std::vector<std::string> segments;   // `path`'s segments
};

// What `url`, written in a page in `folder` (its segments below pages/),
// requests. Nullopt for URLs that aren't local paths: with a scheme
// ("https:", "mailto:"), "//host/...", or only "#x" or "?q". With `folder`
// unknown (a fragment, which can go into any page), only paths from the root
// ("/x") are local. Sets `above` for a relative path that climbs out of the
// site.
std::optional<LocalUrl> localUrl(const std::optional<std::vector<std::string>>& folder, const std::string& url,
                                 bool& above);

// Whether some route serves this site path (either form of a trailing slash).
bool routeExists(const CompiledApp& app, const std::string& path);

// What's wrong with `url`, if it's local and nothing serves it: "there's no
// such page or file", maybe with a hint ("pages are served without .html:
// write \"earn\" for \"earn.html\""). Empty if it's fine, or not local.
// `path` is set to the site path it resolved to.
std::string localLinkProblem(const CompiledApp& app, const std::optional<std::vector<std::string>>& folder,
                             const std::string& url, std::string* path = nullptr);

}  // namespace fourfold
