// The links pass: every local URL written literally in an HTML template
// (href="about", src="img/logo.png", hx-get="/guestbook/more", ...) must be
// something the app serves. Each one that isn't is a warning, at the file
// and line where it's written, since a typo'd or stale link is otherwise
// only found by clicking it.
//
// A page's relative URLs resolve against its own folder, which is also its
// URL's folder (canonical URLs make that true; overview §4, Routing). That
// includes the links of fragments it includes. A fragment rendered from code
// can go into any page, as can the 404 and 500 pages and the Markdown
// layout, so for those only URLs from the root ("/x") are checked.

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

#include "links.h"
#include "passes.h"

namespace fourfold {

namespace {

// One broken URL path in one file: where it first appears, and how often.
struct Broken {
    int line = 0;
    std::string attribute, url, problem;
    int more = 0;
};

struct Checker {
    const CompiledApp& app;
    std::set<std::string> files;   // every file in code/pages/, as a site path: "/a/b.png"
    // By file, then by the site path it resolves to, so a link repeated with
    // different queries (index.html?a=1, index.html?a=2, ...) is one warning.
    std::map<std::string, std::map<std::string, Broken>> broken;
    std::set<std::tuple<std::string, int, std::string>> seen;   // file, line, url: once, however included

    void check(const CompiledTemplate& t, const std::optional<std::vector<std::string>>& folder) {
        for (const CompiledTemplate::Link& link : t.links()) {
            if (!seen.insert({link.file, link.line, link.url}).second) continue;
            std::string path;
            std::string problem = localLinkProblem(app, folder, link.url, &path);
            if (problem.empty()) continue;
            // A file that's there but not served: say why.
            auto canonical = canonicalUrlPath(path);
            if (canonical && files.count(canonical.value().path)) {
                size_t dot = path.rfind('.');
                std::string ext = dot == std::string::npos ? "" : path.substr(dot);
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".ms") {
                    problem = "that's a page's code, which is never served";
                } else if (ext.empty()) {
                    problem = "the file is there, but files without an extension aren't served";
                } else if (ext != ".html" && ext != ".md") {   // pages: the hint says what to write
                    problem = "the file is there, but " + ext + " files aren't served (not a known type)";
                }
            }
            auto [it, isNew] = broken[link.file].try_emplace(path, Broken{link.line, link.attribute, link.url, problem});
            if (!isNew) it->second.more++;
        }
    }

    void report(Diagnostics& diagnostics) const {
        for (const auto& [file, paths] : broken) {
            std::vector<const Broken*> inOrder;
            for (const auto& entry : paths) inOrder.push_back(&entry.second);
            std::sort(inOrder.begin(), inOrder.end(), [](const Broken* a, const Broken* b) { return a->line < b->line; });
            for (const Broken* found : inOrder) {
                const Broken& b = *found;
                std::string message = b.attribute + "=\"" + b.url + "\": " + b.problem;
                if (b.more > 0) {
                    message += " (and " + std::to_string(b.more) + " more like it in this file)";
                }
                diagnostics.warning("code/" + file, message, b.line);
            }
        }
    }
};

}  // namespace

void linksPass(const CodeListing& listing, const CompiledApp& app, Diagnostics& diagnostics) {
    Checker checker{app, {}, {}, {}};
    const RelPath pages = RelPath::parse("pages").value();
    for (const CodeEntry& entry : listing.files) {
        if (entry.path.isWithin(pages)) checker.files.insert(entry.path.str().substr(5));   // "/a/b.png"
    }
    for (const auto& [url, route] : app.routes) {
        if (route.kind != RouteKind::Page) continue;
        const CompiledTemplate* t = app.templates.find(route.source);
        if (!t) continue;   // a page made only of code
        std::vector<std::string> folder;
        RelPath parent = route.source.parent();   // segments() views into it
        std::vector<std::string_view> segments = parent.segments();
        for (size_t i = 1; i < segments.size(); i++) folder.emplace_back(segments[i]);
        checker.check(*t, folder);
    }
    for (const std::optional<RelPath>* special : {&app.notFoundPage, &app.errorPage}) {
        if (!*special) continue;
        if (const CompiledTemplate* t = app.templates.find(**special)) checker.check(*t, std::nullopt);
    }
    if (app.markdownLayout) checker.check(*app.markdownLayout, std::nullopt);
    for (const auto& [name, t] : app.templates.fragments()) checker.check(t, std::nullopt);
    checker.report(diagnostics);
}

}  // namespace fourfold
