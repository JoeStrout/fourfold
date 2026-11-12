// The scripts pass: .ms files in code/pages/ are page code (render and
// handlers); .ms files directly in code/lib/ are modules for `import`; .ms
// files in code/jobs/ define jobs.
// Compiling them is the script component's job; this pass picks the files.
// (code/access.ms and code/limits.ms are declaration files, which the access
// and limits passes read.)

#include "passes.h"

namespace fourfold {

std::map<RelPath, PageHandlers> scriptsPass(const CodeListing& listing, const CodeTree& code,
                                            ScriptCompiler* compiler, DatabaseSources database, CompiledApp& app,
                                            Diagnostics& diagnostics) {
    const RelPath migrations = RelPath::parse("migrations").value();
    const RelPath pages = RelPath::parse("pages").value();
    const RelPath lib = RelPath::parse("lib").value();
    const RelPath jobs = RelPath::parse("jobs").value();
    std::vector<CodeFile> pageFiles, libFiles, jobFiles;

    for (const CodeEntry& entry : listing.files) {
        if (entry.path.extension() != ".ms") continue;
        if (entry.path.str() == "access.ms" || entry.path.str() == "limits.ms" || entry.path.str() == "outbound.ms" ||
            entry.path.str() == "schedule.ms") {
            continue;   // declarations, evaluated as data by their passes
        }
        if (entry.path.str() == "seed.ms" || entry.path.isWithin(migrations)) continue;   // the database pass's
        std::string name = codeFileName(entry.path);
        std::vector<CodeFile>* into = nullptr;
        if (entry.path.isWithin(pages)) {
            into = &pageFiles;
        } else if (entry.path.isWithin(jobs)) {
            into = &jobFiles;
        } else if (entry.path.parent() == lib) {
            into = &libFiles;
        } else if (entry.path.isWithin(lib)) {
            diagnostics.warning(name, "can't be imported: only files directly in code/lib/ can; ignored");
        } else {
            diagnostics.warning(name,
                                "MiniScript files belong in code/pages/, code/lib/, code/jobs/ or code/migrations/; ignored");
        }
        if (!into) continue;
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(name, file.error().message);
            continue;
        }
        into->push_back(std::move(file).value());
    }

    if (pageFiles.empty() && libFiles.empty() && jobFiles.empty() && database.migrations.empty() && !database.seed) {
        return {};
    }
    if (!compiler) {
        diagnostics.error("code", "this app has MiniScript files, but no script compiler was provided");
        return {};
    }
    ScriptSources sources{std::move(pageFiles), std::move(libFiles), std::move(database.migrations),
                          std::move(database.seed), std::move(jobFiles)};
    CompiledScripts compiled = compiler->compileScripts(sources);
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
