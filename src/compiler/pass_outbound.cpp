// The outbound pass: where the app may send requests and mail (overview §6,
// Destination allowlist), from code/outbound.ms, evaluated as data:
//
//   allow  = ["api.weather.gov", "*.stripe.com", "http://feeds.example.org"]
//   mailTo = ["owner@example.com"]
//
// No outbound.ms means no outbound requests, and mail only to the app's
// users. The config can tighten this (config.outbound), never widen it.

#include "passes.h"

namespace fourfold {

namespace {

const char* const kOutboundFile = "outbound.ms";

// A list of strings from `global`, or nullopt (with an error reported) if
// it isn't one. A global the file doesn't set is an empty list.
std::optional<std::vector<std::string>> readList(const CodeFile& file, const std::string& global,
                                                 DataEvaluator& evaluator, const std::string& fileName,
                                                 Diagnostics& diagnostics, bool& set) {
    EvaluatedData d = evaluator.evaluate(file, global);
    if (d.error) {
        diagnostics.error(d.error->file.empty() ? fileName : d.error->file, d.error->message, d.error->line);
        return std::nullopt;
    }
    std::vector<std::string> out;
    if (d.value.isNull()) return out;
    set = true;
    bool ok = d.value.kind() == Datum::Kind::List;
    if (ok) {
        for (const Datum& item : d.value.asList()) {
            ok = ok && item.kind() == Datum::Kind::String;
            if (ok) out.push_back(item.asString());
        }
    }
    if (!ok) {
        diagnostics.error(fileName, "`" + global + "` must be a list of strings");
        return std::nullopt;
    }
    return out;
}

}  // namespace

void outboundPass(const CodeListing& listing, const CodeTree& code, DataEvaluator* evaluator, CompiledApp& app,
                  Diagnostics& diagnostics) {
    const RelPath path = RelPath::parse(kOutboundFile).value();
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
    bool allowSet = false, mailSet = false;
    auto allow = readList(file.value(), "allow", *evaluator, fileName, diagnostics, allowSet);
    auto mailTo = readList(file.value(), "mailTo", *evaluator, fileName, diagnostics, mailSet);
    if (!allow || !mailTo) return;
    if (!allowSet && !mailSet) {
        diagnostics.warning(fileName, "sets neither `allow` (hosts for http) nor `mailTo` (addresses for mail), so "
                                      "it allows nothing");
    }

    OutboundPolicy policy;
    policy.declared = true;
    for (const std::string& a : *allow) {
        std::string error;
        if (auto rule = parseHostRule(a, error)) {
            policy.hosts.push_back(*rule);
        } else {
            diagnostics.error(fileName, "allow: " + error);
        }
    }
    for (const std::string& m : *mailTo) {
        std::string error;
        if (auto rule = parseMailRule(m, error)) {
            policy.mailTo.push_back(*rule);
        } else {
            diagnostics.error(fileName, "mailTo: " + error);
        }
    }
    if (policy.anyHost()) {
        diagnostics.warning(fileName, "allow has \"*\": app code may send requests to any public host (fine for "
                                      "link previews or feeds; be sure that's meant)");
    }
    if (policy.anyMail()) {
        diagnostics.warning(fileName, "mailTo has \"*\": app code may send mail to any address, not just users "
                                      "(it's limited by config.mail.appPerHour; be sure that's meant)");
    }
    app.outbound = std::move(policy);
}

}  // namespace fourfold
