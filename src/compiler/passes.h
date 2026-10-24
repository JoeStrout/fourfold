// The compiler's passes, each in its own file. A pass reads the listing (and
// files, through the CodeTree), adds to the app under construction, and
// reports problems through the Diagnostics it's given.
#pragma once

#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"

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

// Routes for code/pages/: pages, static files, and the 404 page.
void routesPass(const CodeListing& listing, CompiledApp& app, Diagnostics& diagnostics);

}  // namespace fourfold
