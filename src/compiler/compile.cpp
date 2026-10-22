#include "compiler/compiler.h"

#include "passes.h"

namespace fourfold {

void Diagnostics::error(std::string file, std::string message, int line) {
    all_.push_back({Diagnostic::Severity::Error, std::move(file), line, std::move(message)});
    errors_++;
}

void Diagnostics::warning(std::string file, std::string message, int line) {
    all_.push_back({Diagnostic::Severity::Warning, std::move(file), line, std::move(message)});
}

std::vector<Diagnostic> Diagnostics::warnings() const {
    std::vector<Diagnostic> result;
    for (const Diagnostic& d : all_) {
        if (d.severity == Diagnostic::Severity::Warning) result.push_back(d);
    }
    return result;
}

std::string codeFileName(const RelPath& path) {
    return path.isRoot() ? "code" : "code/" + path.str();
}

CompileOutput compile(const CodeTree& code, const CompileOptions& options) {
    Diagnostics diagnostics;
    auto image = std::make_shared<AppImage>();
    image->generation = options.generation;

    auto listing = code.list();
    if (!listing) {
        diagnostics.error("code", listing.error().message);
        return {nullptr, diagnostics.all()};
    }
    // Skipped entries are listed as "code/<path>: <reason>".
    for (const std::string& skipped : listing.value().skipped) {
        size_t colon = skipped.find(": ");
        diagnostics.warning(skipped.substr(0, colon),
                            colon == std::string::npos ? "skipped" : skipped.substr(colon + 2) + "; ignored");
    }

    routesPass(listing.value(), *image, diagnostics);

    if (diagnostics.hasErrors()) return {nullptr, diagnostics.all()};
    image->diagnostics = diagnostics.warnings();
    return {std::move(image), diagnostics.all()};
}

}  // namespace fourfold
