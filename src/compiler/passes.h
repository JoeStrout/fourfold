// The compiler's passes, each in its own file. A pass reads the listing (and
// files, through the CodeTree), adds to the app under construction, and
// reports problems through the Diagnostics it's given.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"
#include "script/script_host.h"

namespace fourfold {

class Diagnostics {
public:
    void error(std::string file, std::string message, int line = 0);
    void warning(std::string file, std::string message, int line = 0);

    bool hasErrors() const { return errors_ > 0; }
    const std::vector<Diagnostic>& all() const { return all_; }
    std::vector<Diagnostic> warnings() const;

private:
    std::vector<Diagnostic> all_;
    int errors_ = 0;
};

// "code/" + path, as diagnostics name files.
std::string codeFileName(const RelPath& path);

// The path's extension, lowercased (".html").
std::string lowerExtension(const RelPath& path);

// Parses the templates in code/frags/ and code/pages/ into app.templates
// (and the Markdown layout), resolving includes.
void templatesPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics);

// Compiles the .ms files in code/pages/ and code/lib/ into app.scripts, and
// returns the handlers each page file defines.
std::map<RelPath, PageHandlers> scriptsPass(const CodeListing& listing, const CodeTree& code,
                                            ScriptCompiler* compiler, CompiledApp& app, Diagnostics& diagnostics);

// Routes for code/pages/: pages, handlers, static files, and the 404 and 500
// pages.
void routesPass(const CodeListing& listing, const std::map<RelPath, PageHandlers>& handlers, CompiledApp& app,
                Diagnostics& diagnostics);

// Renders the .md pages in code/pages/ into app.markdownPages, rewriting
// links between them and warning about links to routes that don't exist.
// After the routes pass, which it checks links against.
void markdownPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics);

// Warns about each local URL written in a template (href, src, hx-get, ...)
// that no route serves. After the routes pass.
void linksPass(const CodeListing& listing, const CompiledApp& app, Diagnostics& diagnostics);

}  // namespace fourfold
