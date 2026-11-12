// The limits pass: each route's rate limits and spam checks (overview §5,
// Rate limits and spam), from code/limits.ms, evaluated as data:
//
//   limits = {}
//   limits["/guestbook/sign"] = ["5/minute", "50/day"]
//   limits["/api"]            = "100/minute per user"
//   limits["/forum/post"]     = {"rate": "10/hour", "minSeconds": 3, "challenge": true}
//
// A rule covers its path and everything below it, on whole segments, as in
// access.ms. Every rule that applies does (adding one only tightens): a
// route gets each covering rule's rates, the longest minSeconds, the
// shortest maxAgeMinutes, and a challenge if any rule asks for one. The
// routes a rule covers share its counters. A rule that covers no route is a
// warning.

#include <algorithm>

#include "passes.h"

namespace fourfold {

namespace {

const char* const kLimitsFile = "limits.ms";

struct LimitRule {
    std::string path;
    std::vector<RateRule> rates;
    double minSeconds = 0;
    double maxAgeSeconds = 0;
    bool challenge = false;
    bool used = false;
};

bool covers(const std::string& rule, const std::string& url) {
    if (rule == "/") return true;
    return url == rule || (url.size() > rule.size() && url.compare(0, rule.size(), rule) == 0 && url[rule.size()] == '/');
}

// Rates from a string or a list of strings; false (with `error`) if not.
bool readRates(const Datum& value, const std::string& key, std::vector<RateRule>& out, std::string& error) {
    std::vector<Datum> items;
    if (value.kind() == Datum::Kind::String) {
        items.push_back(value);
    } else if (value.kind() == Datum::Kind::List && !value.asList().empty()) {
        items = value.asList();
    } else {
        error = "a rate is a string like \"5/minute\", or a list of them";
        return false;
    }
    for (const Datum& item : items) {
        if (item.kind() != Datum::Kind::String) {
            error = "a rate is a string like \"5/minute\", or a list of them";
            return false;
        }
        std::optional<RateRule> rate = parseRate(item.asString(), error);
        if (!rate) return false;
        rate->key = key;
        out.push_back(*rate);
    }
    return true;
}

std::optional<LimitRule> readRule(const std::string& path, const Datum& value, const std::string& fileName,
                                  std::string& error) {
    LimitRule rule;
    rule.path = path;
    std::string key = fileName + ": " + path;
    if (value.kind() != Datum::Kind::Map) {
        if (!readRates(value, key, rule.rates, error)) return std::nullopt;
        return rule;
    }
    for (const auto& [name, v] : value.asMap()) {
        if (name == "rate") {
            if (!readRates(v, key, rule.rates, error)) return std::nullopt;
        } else if (name == "minSeconds") {
            if (v.kind() != Datum::Kind::Number || !(v.asNumber() > 0 && v.asNumber() <= 3600)) {
                error = "minSeconds must be a number of seconds, more than 0 and at most 3600";
                return std::nullopt;
            }
            rule.minSeconds = v.asNumber();
        } else if (name == "maxAgeMinutes") {
            if (v.kind() != Datum::Kind::Number || !(v.asNumber() >= 1 && v.asNumber() <= 60 * 24 * 30)) {
                error = "maxAgeMinutes must be a number of minutes, from 1 to 43200 (30 days)";
                return std::nullopt;
            }
            rule.maxAgeSeconds = v.asNumber() * 60;
        } else if (name == "challenge") {
            if (v.kind() != Datum::Kind::Number || (v.asNumber() != 0 && v.asNumber() != 1)) {
                error = "challenge must be true or false";
                return std::nullopt;
            }
            rule.challenge = v.asNumber() == 1;
        } else {
            error = "unknown setting \"" + name + "\" (a rule's map has rate, minSeconds, maxAgeMinutes and challenge)";
            return std::nullopt;
        }
    }
    if (rule.maxAgeSeconds > 0 && rule.minSeconds == 0) {
        error = "maxAgeMinutes needs minSeconds: both are part of the timing check";
        return std::nullopt;
    }
    return rule;
}

}  // namespace

void limitsPass(const CodeListing& listing, const CodeTree& code, DataEvaluator* evaluator, CompiledApp& app,
                Diagnostics& diagnostics) {
    const RelPath limitsPath = RelPath::parse(kLimitsFile).value();
    const std::string fileName = codeFileName(limitsPath);
    bool hasFile = false;
    for (const CodeEntry& e : listing.files) hasFile = hasFile || e.path == limitsPath;
    if (!hasFile) return;

    auto file = code.read(limitsPath);
    if (!file) {
        diagnostics.error(fileName, file.error().message);
        return;
    }
    if (!evaluator) {
        diagnostics.error(fileName, "can't be read: no evaluator was provided");
        return;
    }
    EvaluatedData d = evaluator->evaluate(file.value(), "limits");
    if (d.error) {
        diagnostics.error(d.error->file.empty() ? fileName : d.error->file, d.error->message, d.error->line);
        return;
    }
    if (d.value.kind() != Datum::Kind::Map) {
        diagnostics.error(fileName, d.value.isNull() ? "must set `limits`, a map from paths to rates (start with "
                                                       "limits = {})"
                                                     : "`limits` must be a map from paths to rates");
        return;
    }

    std::vector<LimitRule> rules;
    for (const auto& [path, value] : d.value.asMap()) {
        std::string where = "limits[\"" + path + "\"]: ";
        auto canonical = canonicalUrlPath(path);
        if (!canonical || canonical.value().path != path || path.find_first_of("*?#") != std::string::npos) {
            std::string why = path.find('*') != std::string::npos
                                  ? "paths have no wildcards; a rule covers everything below its path"
                              : !path.empty() && path.back() == '/' && path != "/"
                                  ? "write the path without the trailing slash"
                                  : "not a plain URL path like \"/guestbook/sign\"";
            diagnostics.error(fileName, where + why);
            continue;
        }
        std::string error;
        std::optional<LimitRule> rule = readRule(path, value, fileName, error);
        if (!rule) {
            diagnostics.error(fileName, where + error);
            continue;
        }
        rules.push_back(std::move(*rule));
    }

    for (const auto& [url, original] : app.routes) {
        RouteEntry* entry = app.routes.findForUpdate(url);
        for (LimitRule& rule : rules) {
            if (!covers(rule.path, url)) continue;
            rule.used = true;
            entry->limits.insert(entry->limits.end(), rule.rates.begin(), rule.rates.end());
            entry->limitSources.push_back(fileName + ": " + rule.path);
            entry->minSeconds = std::max(entry->minSeconds, rule.minSeconds);
            if (rule.maxAgeSeconds > 0) {
                entry->maxAgeSeconds = entry->maxAgeSeconds > 0 ? std::min(entry->maxAgeSeconds, rule.maxAgeSeconds)
                                                                : rule.maxAgeSeconds;
            }
            entry->challenge = entry->challenge || rule.challenge;
            if (entry->webhook && (rule.minSeconds > 0 || rule.challenge)) {
                diagnostics.warning(fileName, "the rule for " + rule.path + " has " +
                                                  (rule.challenge ? "a challenge" : "minSeconds") + ", which " +
                                                  entry->webhook->label() + " (" + url +
                                                  ") doesn't get: a webhook's caller isn't a browser");
            }
        }
    }
    for (const LimitRule& rule : rules) {
        if (!rule.used) {
            diagnostics.warning(fileName, "the rule for " + rule.path +
                                              " covers no page, handler or file (was a folder renamed?)");
        }
    }
}

}  // namespace fourfold
