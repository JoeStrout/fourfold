// The Markdown pass: each .md page in code/pages/ is parsed, here, into a
// template with an HTML form and a Markdown form (Dynamic Markdown; overview
// §4, Markdown pages and Fields in Markdown), and its includes resolved.
//
// Links between .md files are rewritten to their routes, so a folder of
// Markdown reads correctly on GitHub and when served: [setup](setup.md#x)
// becomes /setup#x. A relative or site-absolute link (or image) to a route
// that doesn't exist is a warning.

#include <algorithm>
#include <cctype>

#include "links.h"
#include "markdown/markdown.h"
#include "passes.h"

namespace fourfold {

namespace {

bool endsWithMd(const std::string& s) {
    if (s.size() < 3) return false;
    std::string tail = s.substr(s.size() - 3);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".md";
}

struct LinkChecker {
    const CompiledApp& app;
    std::vector<std::string> folder;   // the page's folder, below pages/

    std::optional<std::string> rewrite(const std::string& url, bool isImage, std::string& warning) const {
        bool above = false;
        std::optional<LocalUrl> local = localUrl(folder, url, above);
        if (above) {
            warning = "links to " + url + ": it climbs above the site's root";
            return std::nullopt;
        }
        if (!local) return std::nullopt;   // off-site, or on this page
        if (!isImage && !local->segments.empty() && endsWithMd(local->segments.back())) {
            // A link to another .md page: to its route.
            std::vector<std::string> segments = local->segments;
            std::string stem = segments.back().substr(0, segments.back().size() - 3);
            segments.pop_back();
            bool index = stem == "index";
            if (!index) segments.push_back(stem);
            std::string target;
            for (const std::string& s : segments) target += "/" + s;
            if (target.empty()) target = "/";
            if (!routeExists(app, target)) {
                warning = "links to " + url + ": there's no such page";
                return std::nullopt;
            }
            return target + (index && target != "/" ? "/" : "") + local->rest;
        }
        std::string problem = localLinkProblem(app, folder, url);
        if (!problem.empty()) warning = (isImage ? "shows " : "links to ") + url + ": " + problem;
        return std::nullopt;
    }
};

}  // namespace

void markdownPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(pages)) continue;
        if (entry.path.str() == "pages/markdown.css") app.hasMarkdownCss = true;
        if (lowerExtension(entry.path) != ".md") continue;
        std::optional<std::string> routedUrl;
        for (const auto& [url, route] : app.routes) {
            if (route.kind == RouteKind::Markdown && route.source == entry.path) routedUrl = url;
        }
        if (!routedUrl) continue;   // not routed (the routes pass said why)

        auto file = code.read(entry.path);
        std::string fileName = codeFileName(entry.path);
        if (!file) {
            diagnostics.error(fileName, file.error().message);
            continue;
        }
        LinkChecker checker{app, {}};
        RelPath folder = entry.path.parent();   // segments() views into it
        std::vector<std::string_view> segments = folder.segments();
        for (size_t i = 1; i < segments.size(); i++) checker.folder.emplace_back(segments[i]);

        MarkdownOptions options;
        options.profile = MarkdownProfile::Full;
        options.rewriteUrl = [&checker](const std::string& url, bool isImage, std::string& warning) {
            return checker.rewrite(url, isImage, warning);
        };
        std::vector<std::string> warnings;
        ParsedTemplate parsed = parseMarkdownTemplate(file.value(), TemplateKind::Page, options, &warnings);
        for (const TemplateProblem& p : parsed.errors) diagnostics.error("code/" + p.file, p.message, p.line);
        if (!parsed.ok()) continue;
        for (const std::string& w : warnings) diagnostics.warning(fileName, w);
        std::vector<TemplateProblem> problems = resolveIncludes(parsed.compiled, app.templates.fragments());
        for (const TemplateProblem& p : problems) diagnostics.error("code/" + p.file, p.message, p.line);
        if (!problems.empty()) continue;

        // A {{flash}} field shows the session's flash message, so the route
        // uses the session (as for an HTML page).
        const std::vector<std::string>& fields = parsed.compiled.fieldNames();
        if (std::find(fields.begin(), fields.end(), "flash") != fields.end()) {
            if (RouteEntry* route = app.routes.findForUpdate(*routedUrl)) route->usesSession = true;
        }
        app.templates.add(entry.path, std::move(parsed.compiled));
    }
}

}  // namespace fourfold
