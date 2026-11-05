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

// What the database pass hands the scripts pass: migrations' MiniScript and
// code/seed.ms.
struct DatabaseSources {
    std::vector<MigrationScript> migrations;
    std::optional<CodeFile> seed;
};

// Named queries (code/queries/), migrations (code/migrations/) and the seed
// (code/seed.ms), for `dialect`, into the app.
DatabaseSources databasePass(const CodeListing& listing, const CodeTree& code, SqlDialect dialect, CompiledApp& app,
                             Diagnostics& diagnostics);

// Compiles the .ms files in code/pages/ and code/lib/ into app.scripts, with
// the migrations' and the seed's, and returns the handlers each page file
// defines.
std::map<RelPath, PageHandlers> scriptsPass(const CodeListing& listing, const CodeTree& code,
                                            ScriptCompiler* compiler, DatabaseSources database, CompiledApp& app,
                                            Diagnostics& diagnostics);

// Routes for code/pages/: pages, handlers, static files, and the 404 and 500
// pages.
void routesPass(const CodeListing& listing, const std::map<RelPath, PageHandlers>& handlers, CompiledApp& app,
                Diagnostics& diagnostics);

// Parses the .md pages in code/pages/ into app.templates, rewriting links
// between them and warning about links to routes that don't exist. After the
// routes pass, which it checks links against, and the templates pass, whose
// fragments they include.
void markdownPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics);

// Each route's effective access, from code/access.ms (evaluated by
// `evaluator`) and the page files' `access =`. After the routes pass.
void accessPass(const CodeListing& listing, const CodeTree& code, const std::map<RelPath, PageHandlers>& handlers,
                DataEvaluator* evaluator, CompiledApp& app, Diagnostics& diagnostics);

// Each route's rate limits, timing check and challenge, from code/limits.ms
// (evaluated by `evaluator`). After the routes pass.
void limitsPass(const CodeListing& listing, const CodeTree& code, DataEvaluator* evaluator, CompiledApp& app,
                Diagnostics& diagnostics);

// The page files' form declarations, onto their post handlers' routes; then
// each template's forms checked against them (with validation attributes
// added), and marked for a proof-of-work puzzle where their route has a
// challenge. After the routes, access and limits passes.
void formsPass(const std::map<RelPath, PageHandlers>& handlers, CompiledApp& app, Diagnostics& diagnostics);

// An access level as written ("login", "admin", ["admin", "moderator"]): the
// roles of which any will do, empty for "login"; or nullopt, with `error`
// saying why it isn't one.
std::optional<std::vector<std::string>> parseAccessValue(const Datum& value, std::string& error);

// Warns about each local URL written in a template (href, src, hx-get, ...)
// that no route serves. After the routes pass.
void linksPass(const CodeListing& listing, const CompiledApp& app, Diagnostics& diagnostics);

}  // namespace fourfold
