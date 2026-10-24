// The scripts pass: .ms files in code/pages/ are page code (render and
// handlers); .ms files directly in code/lib/ are modules for `import`.
// Compiling them is the script component's job; this pass picks the files.

#include "passes.h"

namespace fourfold {

std::map<RelPath, PageHandlers> scriptsPass(const CodeListing& listing, const CodeTree& code,
                                            ScriptCompiler* compiler, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    const RelPath lib = RelPath::parse("lib").value();
    std::vector<CodeFile> pageFiles, libFiles;

    for (const CodeEntry& entry : listing.files) {
        if (entry.path.extension() != ".ms") continue;
        std::string name = codeFileName(entry.path);
        std::vector<CodeFile>* into = nullptr;
        if (entry.path.isWithin(pages)) {
            into = &pageFiles;
        } else if (entry.path.parent() == lib) {
            into = &libFiles;
        } else if (entry.path.isWithin(lib)) {
            diagnostics.warning(name, "can't be imported: only files directly in code/lib/ can; ignored");
        } else {
            diagnostics.warning(name, "MiniScript files belong in code/pages/ or code/lib/; ignored");
        }
        if (!into) continue;
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(name, file.error().message);
            continue;
        }
        into->push_back(std::move(file).value());
    }

    if (pageFiles.empty() && libFiles.empty()) return {};
    if (!compiler) {
        diagnostics.error("code", "this app has MiniScript files, but no script compiler was provided");
        return {};
    }
    CompiledScripts compiled = compiler->compileScripts(pageFiles, libFiles);
    for (const Diagnostic& d : compiled.diagnostics) {
        if (d.severity == Diagnostic::Severity::Error) {
            diagnostics.error(d.file, d.message, d.line);
        } else {
            diagnostics.warning(d.file, d.message, d.line);
        }
    }
    app.scripts = compiled.bundle;
    return compiled.pages;
}

}  // namespace fourfold
