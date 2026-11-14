// The static pass (M11.2): code/static.ms, evaluated as data, says what else
// is served as a static file, and how long static files may be cached:
//
//   types = {}
//   types["unityweb"] = "application/octet-stream"
//   types["xml"] = "application/rss+xml"
//
//   maxAge = {}
//   maxAge["/MiniMicro/Build"] = 60 * 60 * 24    // seconds
//
// Built-in types can't be changed, and types that would run script as the
// site can't be added (see checkAddedType). A maxAge rule covers its path and
// everything below it, on whole segments; the most specific rule for a file
// wins. Without one, static files are revalidated on every use (no-cache).
// Before the routes pass, which uses both.

#include "compiler/content_types.h"
#include "passes.h"

namespace fourfold {

namespace {

const char* const kStaticFile = "static.ms";
constexpr int64_t kMaxAgeLimit = 365LL * 24 * 60 * 60;   // a year, HTTP's practical most

}  // namespace

void staticPass(const CodeListing& listing, const CodeTree& code, DataEvaluator* evaluator, CompiledApp& app,
                Diagnostics& diagnostics) {
    const RelPath path = RelPath::parse(kStaticFile).value();
    const std::string fileName = codeFileName(path);
    bool hasFile = false;
    for (const CodeEntry& e : listing.files) hasFile = hasFile || e.path == path;
    if (!hasFile) return;

    auto file = code.read(path);
    if (!file) {
        diagnostics.error(fileName, file.error().message);
        return;
    }
    if (!evaluator) {
        diagnostics.error(fileName, "can't be read: no evaluator was provided");
        return;
    }
    EvaluatedData types = evaluator->evaluate(file.value(), "types");
    EvaluatedData maxAge = types.error ? EvaluatedData{} : evaluator->evaluate(file.value(), "maxAge");
    for (const EvaluatedData* d : {&types, &maxAge}) {
        if (d->error) {
            diagnostics.error(d->error->file.empty() ? fileName : d->error->file, d->error->message, d->error->line);
            return;
        }
    }
    if (types.value.isNull() && maxAge.value.isNull()) {
        diagnostics.error(fileName, "must set `types` (file types to serve) or `maxAge` (how long files may be "
                                    "cached), or both (start with types = {})");
        return;
    }

    if (!types.value.isNull()) {
        if (types.value.kind() != Datum::Kind::Map) {
            diagnostics.error(fileName, "`types` must be a map from file extensions to content types");
        } else {
            for (const auto& [extension, value] : types.value.asMap()) {
                std::string where = "types[\"" + extension + "\"]: ";
                if (value.kind() != Datum::Kind::String) {
                    diagnostics.error(fileName, where + "a content type is a string, like \"application/octet-stream\"");
                    continue;
                }
                std::string contentType = value.asString();
                std::string error;
                bool sandbox = false;
                std::string ext = checkAddedType(extension, contentType, sandbox, error);
                if (ext.empty()) {
                    diagnostics.error(fileName, where + error);
                    continue;
                }
                if (!app.addedTypes.emplace(ext, AddedType{contentType, sandbox}).second) {
                    diagnostics.error(fileName, where + ext + " is given twice");
                }
            }
        }
    }

    if (!maxAge.value.isNull()) {
        if (maxAge.value.kind() != Datum::Kind::Map) {
            diagnostics.error(fileName, "`maxAge` must be a map from paths to numbers of seconds");
            return;
        }
        for (const auto& [rulePath, value] : maxAge.value.asMap()) {
            std::string where = "maxAge[\"" + rulePath + "\"]: ";
            auto canonical = canonicalUrlPath(rulePath);
            if (!canonical || canonical.value().path != rulePath || rulePath.find_first_of("*?#{") != std::string::npos) {
                std::string why = !rulePath.empty() && rulePath.back() == '/' && rulePath != "/"
                                      ? "write the path without the trailing slash"
                                      : "not a plain URL path like \"/MiniMicro/Build\" (a rule covers everything "
                                        "below its path)";
                diagnostics.error(fileName, where + why);
                continue;
            }
            if (value.kind() != Datum::Kind::Number || !(value.asNumber() >= 0 && value.asNumber() <= kMaxAgeLimit) ||
                value.asNumber() != static_cast<double>(static_cast<int64_t>(value.asNumber()))) {
                diagnostics.error(fileName, where + "must be a whole number of seconds, from 0 to " +
                                                std::to_string(kMaxAgeLimit) + " (a year)");
                continue;
            }
            app.cacheRules.push_back({rulePath, static_cast<int64_t>(value.asNumber())});
        }
    }
}

}  // namespace fourfold
