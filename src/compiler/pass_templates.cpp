// The templates pass: every .html file in code/pages/ is parsed once, at
// compile time, so a template that breaks the field rules never deploys.

#include "passes.h"

namespace fourfold {

void templatesPass(const CodeListing& listing, const CodeTree& code, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(pages)) continue;
        if (lowerExtension(entry.path) != ".html") continue;
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(codeFileName(entry.path), file.error().message);
            continue;
        }
        ParsedTemplate parsed = parseTemplate(file.value());
        for (const TemplateProblem& p : parsed.errors) {
            diagnostics.error(codeFileName(entry.path), p.message, p.line);
        }
        if (parsed.ok()) app.templates.add(entry.path, std::move(parsed.compiled));
    }
}

}  // namespace fourfold
