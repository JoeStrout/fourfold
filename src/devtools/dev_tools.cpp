#include "devtools/dev_tools.h"

#include <cstdio>

#include <algorithm>
#include <cctype>
#include <vector>

#include "template/template.h"

namespace fourfold {

namespace {

constexpr auto kHeartbeat = std::chrono::seconds(20);
constexpr int kExcerptContext = 3;   // lines before and after
constexpr size_t kMaxWarnings = 50;   // render warnings kept per generation

Datum str(std::string s) { return Datum::string(std::move(s)); }
Datum num(double n) { return Datum::number(n); }

// The lines around `line` (1-based) of `source`: {first, lines}.
struct Excerpt {
    int first = 0;
    std::vector<std::string> lines;
};

Excerpt excerptOf(const std::string& source, int line) {
    Excerpt e;
    if (line <= 0) return e;
    e.first = std::max(1, line - kExcerptContext);
    int last = line + kExcerptContext;
    int n = 1;
    size_t pos = 0;
    while (pos <= source.size() && n <= last) {
        size_t end = source.find('\n', pos);
        if (end == std::string::npos) end = source.size();
        if (n >= e.first) {
            std::string text = source.substr(pos, end - pos);
            if (!text.empty() && text.back() == '\r') text.pop_back();
            e.lines.push_back(std::move(text));
        }
        if (end == source.size()) break;
        pos = end + 1;
        n++;
    }
    if (e.lines.empty()) e.first = 0;
    return e;
}

// "code/pages/x.ms" → the file's contents, if it's readable.
std::optional<std::string> readCodeFile(const CodeTree& code, const std::string& file) {
    if (file.compare(0, 5, "code/") != 0) return std::nullopt;
    auto path = RelPath::parse(file.substr(5));
    if (!path) return std::nullopt;
    auto read = code.read(path.value());
    if (!read) return std::nullopt;
    return read.value().contents();
}

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The URL a changed stylesheet in pages/ is served at.
std::optional<std::string> stylesheetUrl(const RelPath& path) {
    const std::string& p = path.str();
    if (p.compare(0, 6, "pages/") != 0) return std::nullopt;
    return encodeUrlPath("/" + p.substr(6));
}

const char* const kErrorPageStyle = R"css(
body { margin: 0; font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
       background: #1b1b1f; color: #eee; }
main { max-width: 60rem; margin: 0 auto; padding: 2rem 1.25rem; }
h1 { font-size: 1.15rem; color: #ff8a80; margin: 0 0 .25rem; }
.where { color: #aaa; font-family: ui-monospace, Menlo, monospace; margin: 0 0 1rem; }
.message { font-size: 1.05rem; margin: 0 0 1rem; white-space: pre-wrap; }
pre { background: #111; border-radius: 6px; padding: .75rem 0; overflow-x: auto;
      font: 13px/1.5 ui-monospace, Menlo, monospace; }
pre span { display: block; padding: 0 1rem; white-space: pre; }
pre span.hit { background: #5c1f1f; }
pre i { display: inline-block; width: 3.5em; color: #777; font-style: normal; user-select: none; }
.note { color: #888; font-size: .85rem; }
)css";

const char* const kReportStyle = R"css(
body { margin: 0; font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #fafafa; color: #222; }
main { max-width: 64rem; margin: 0 auto; padding: 2rem 1.25rem; }
h1 { font-size: 1.3rem; }
h2 { font-size: 1.1rem; margin-top: 1.5rem; }
table { border-collapse: collapse; width: 100%; background: #fff; }
th, td { text-align: left; padding: .35rem .6rem; border-bottom: 1px solid #e3e3e3; vertical-align: top; }
td:first-child { font-family: ui-monospace, Menlo, monospace; }
tr.restricted td:nth-child(3) { font-weight: 600; color: #8a3b00; }
.note { color: #666; }
.warning { background: #fff4d6; padding: .4rem .75rem; border-radius: 4px; }
.done { background: #e3f4e1; padding: .4rem .75rem; border-radius: 4px; }
form { margin: 1rem 0; }
)css";

}  // namespace

std::string serverSentEvent(const std::string& name, const Datum& data) {
    // toJson never emits a line break, so the data fits on one line.
    return "event: " + name + "\ndata: " + toJson(data) + "\n\n";
}

void insertBeforeBodyEnd(std::string& html, const std::string& tag) {
    std::string lower = lowered(html);
    size_t at = lower.rfind("</body");
    if (at == std::string::npos) {
        html += tag;
    } else {
        html.insert(at, tag);
    }
}

// The event streams, and what new ones are told first. Everything but
// `state` is on the loop's thread.
struct DevTools::Hub : std::enable_shared_from_this<Hub> {
    explicit Hub(EventLoop& l) : loop(l) {}

    EventLoop& loop;
    std::vector<std::shared_ptr<StreamWriter>> streams;
    bool heartbeatRunning = false;

    // The latest compile, for each new stream's "hello", and the render
    // warnings seen since. Any thread.
    std::mutex mutex;
    Datum hello = Datum::map({{"generation", num(0)}, {"diagnostics", Datum::list({})}});
    uint64_t warningsGeneration = 0;
    std::vector<RenderWarning> warnings;
    // The latest requests counted against rate limits, and refusals, newest
    // last (for /__dev/limits).
    std::vector<std::string> recentLimits;
    // The latest outbound requests, newest last (for /__dev/outbound).
    std::vector<std::string> recentOutbound;

    Datum warningList() const {
        Datum::ListItems list;
        for (const RenderWarning& w : warnings) {
            list.push_back(Datum::map(
                {{"file", str("code/" + w.file)}, {"line", num(w.line)}, {"message", str(w.message)}}));
        }
        return Datum::list(std::move(list));
    }

    void add(std::shared_ptr<StreamWriter> writer) {
        Datum greeting;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Datum::MapItems items = hello.asMap();
            items["warnings"] = warningList();
            greeting = Datum::map(std::move(items));
        }
        if (!writer->write(serverSentEvent("hello", greeting))) return;
        streams.push_back(std::move(writer));
        startHeartbeat();
    }

    void broadcast(const std::string& event) {
        streams.erase(std::remove_if(streams.begin(), streams.end(),
                                     [&](const std::shared_ptr<StreamWriter>& w) { return !w->write(event); }),
                      streams.end());
    }

    // A comment now and then, so proxies and browsers keep the stream open,
    // and closed ones are noticed.
    void startHeartbeat() {
        if (heartbeatRunning) return;
        heartbeatRunning = true;
        std::weak_ptr<Hub> weak = shared_from_this();
        loop.addTimer(std::chrono::duration_cast<std::chrono::milliseconds>(kHeartbeat), [weak] {
            auto hub = weak.lock();
            if (!hub) return;
            hub->heartbeatRunning = false;
            hub->broadcast(": ping\n\n");
            if (!hub->streams.empty()) hub->startHeartbeat();
        });
    }
};

DevTools::DevTools(EventLoop& loop, CodeTree code) : code_(std::move(code)), hub_(std::make_shared<Hub>(loop)) {}

DevTools::~DevTools() = default;

size_t DevTools::streamCount() const {
    return hub_->streams.size();
}

Datum DevTools::describeErrors(const std::vector<Diagnostic>& diagnostics) const {
    Datum::ListItems list;
    for (const Diagnostic& d : diagnostics) {
        if (d.severity != Diagnostic::Severity::Error) continue;
        Datum::MapItems item{{"file", str(d.file)}, {"line", num(d.line)}, {"message", str(d.message)}};
        if (auto source = readCodeFile(code_, d.file)) {
            Excerpt e = excerptOf(*source, d.line);
            if (e.first > 0) {
                Datum::ListItems lines;
                for (std::string& l : e.lines) lines.push_back(str(std::move(l)));
                item["first"] = num(e.first);
                item["code"] = Datum::list(std::move(lines));
            }
        }
        list.push_back(Datum::map(std::move(item)));
    }
    return Datum::list(std::move(list));
}

void DevTools::compileFinished(const CompileReport& report) {
    Datum errors = report.ok ? Datum::list({}) : describeErrors(report.diagnostics);
    std::string event;
    if (!report.ok) {
        event = serverSentEvent("errors", Datum::map({{"generation", num(report.generation)}, {"diagnostics", errors}}));
    } else if (report.cssOnly) {
        Datum::ListItems urls;
        for (const RelPath& p : report.changed) {
            if (auto url = stylesheetUrl(p)) urls.push_back(str(*url));
        }
        event = serverSentEvent("css", Datum::map({{"generation", num(report.generation)},
                                                   {"urls", Datum::list(std::move(urls))}}));
    } else {
        event = serverSentEvent("reload", Datum::map({{"generation", num(report.generation)}}));
    }
    {
        std::lock_guard<std::mutex> lock(hub_->mutex);
        hub_->hello = Datum::map({{"generation", num(report.generation)}, {"diagnostics", errors}});
        if (report.generation != hub_->warningsGeneration) {
            hub_->warningsGeneration = report.generation;
            hub_->warnings.clear();
        }
    }
    std::shared_ptr<Hub> hub = hub_;
    hub->loop.post([hub, event] { hub->broadcast(event); });
}

namespace {

constexpr size_t kRecentLimits = 50;

// A route's limits and spam checks, as the report shows them.
Datum::ListItems limitsOf(const RouteEntry& entry) {
    Datum::ListItems out;
    for (const RateRule& r : entry.limits) out.push_back(str(r.describe() + " (" + r.key + ")"));
    if (entry.minSeconds > 0) {
        std::string t = "minSeconds " + std::to_string(static_cast<int64_t>(entry.minSeconds));
        if (entry.maxAgeSeconds > 0) t += ", maxAgeMinutes " + std::to_string(static_cast<int64_t>(entry.maxAgeSeconds / 60));
        out.push_back(str(t));
    }
    if (entry.challenge) out.push_back(str("challenge (proof of work)"));
    return out;
}

// A post handler's form: its fields, or a note that it reads req.form
// undeclared.
std::string formOf(const RouteEntry& entry) {
    if (!entry.post) return "";
    if (!entry.form) return "none declared: req.form is raw strings";
    std::string out = entry.form->label() + ":";
    for (const FormField& f : entry.form->fields) {
        out += " " + f.name + " (" + formFieldTypeName(f.type) + (f.required ? ", required" : "") + ")";
    }
    return out;
}

}  // namespace

void DevTools::scriptError(const ScriptError& error) {
    // A refusal by the gate (rate limit, timing, puzzle) goes in the limits
    // page's list too.
    if (error.message.compare(0, 17, "request refused: ") != 0) return;
    std::lock_guard<std::mutex> lock(hub_->mutex);
    hub_->recentLimits.push_back(error.message);
    if (hub_->recentLimits.size() > kRecentLimits) hub_->recentLimits.erase(hub_->recentLimits.begin());
}

void DevTools::outboundCall(const OutboundCall& call) {
    std::string line = call.method + " " + call.url;
    if (call.refused.empty()) {
        line += " → " + std::to_string(call.status) + " (" + std::to_string(call.durationMs) + " ms, " +
                std::to_string(call.bytes) + " bytes" +
                (call.redirects ? ", " + std::to_string(call.redirects) + " redirects" : "") + ")";
    } else {
        line += " → refused: " + call.refused;
    }
    std::lock_guard<std::mutex> lock(hub_->mutex);
    hub_->recentOutbound.push_back(line);
    if (hub_->recentOutbound.size() > kRecentLimits) hub_->recentOutbound.erase(hub_->recentOutbound.begin());
}

void DevTools::limitsCounted(const std::string& request, const std::vector<std::pair<std::string, double>>& counts) {
    std::string line = request + ":";
    for (const auto& [limit, count] : counts) {
        char n[32];
        std::snprintf(n, sizeof n, "%.1f", count);
        line += " " + limit + " = " + n + ";";
    }
    line.pop_back();
    std::lock_guard<std::mutex> lock(hub_->mutex);
    hub_->recentLimits.push_back(line);
    if (hub_->recentLimits.size() > kRecentLimits) hub_->recentLimits.erase(hub_->recentLimits.begin());
}

Datum DevTools::accessReport(const CompiledApp& app) {
    Datum::ListItems routes;
    for (const auto& [url, entry] : app.routes) {
        Datum::ListItems sources;
        for (const std::string& source : entry.accessSources) sources.push_back(str(source));
        Datum row = Datum::map({{"url", str(url + (entry.trailingSlash && url != "/" ? "/" : ""))},
                                {"kind", str(routeKindName(entry.kind))},
                                {"access", str(entry.access.describe())},
                                {"sources", Datum::list(sources)},
                                {"limits", Datum::list(limitsOf(entry))},
                                {"form", str(formOf(entry))}});
        routes.push_back(std::move(row));
        // A Markdown page's Markdown URL: the same route, so the same access.
        if (entry.kind == RouteKind::Markdown) {
            routes.push_back(Datum::map({{"url", str(markdownUrlFor(url, entry.trailingSlash))},
                                         {"kind", str("markdown (as Markdown)")},
                                         {"access", str(entry.access.describe())},
                                         {"sources", Datum::list(std::move(sources))},
                                         {"limits", Datum::list(limitsOf(entry))},
                                         {"form", str("")}}));
        }
    }
    Datum::ListItems warnings;
    for (const Diagnostic& d : app.diagnostics) {
        bool forms = d.message.find(" posts to ") != std::string::npos || d.message.find("this form posts") == 0;
        if (d.file == "code/access.ms" || d.file == "code/limits.ms" || d.message.compare(0, 7, "access:") == 0 ||
            forms) {
            warnings.push_back(str(d.toString()));
        }
    }
    return Datum::map({{"generation", num(static_cast<double>(app.generation))},
                       {"routes", Datum::list(std::move(routes))},
                       {"warnings", Datum::list(std::move(warnings))}});
}

Datum DevTools::outboundReport(const CompiledApp& app, Datum secrets) {
    const OutboundPolicy& p = app.outbound;
    Datum::ListItems allow, mailTo, webhooks, warnings;
    for (const HostRule& r : p.hosts) allow.push_back(str(r.text));
    for (const std::string& m : p.mailTo) mailTo.push_back(str(m));
    for (const auto& [url, entry] : app.routes) {
        if (!entry.webhook) continue;
        webhooks.push_back(Datum::map({{"url", str(url)},
                                       {"handler", str(entry.webhook->label() + " in code/" + entry.webhook->source.str())},
                                       {"access", str(entry.access.describe())}}));
    }
    for (const Diagnostic& d : app.diagnostics) {
        if (d.file == "code/outbound.ms" || d.message.find("webhook") != std::string::npos) {
            warnings.push_back(str(d.toString()));
        }
    }
    if (secrets.kind() != Datum::Kind::List) secrets = Datum::list({});
    return Datum::map({{"generation", num(static_cast<double>(app.generation))},
                       {"declared", num(p.declared ? 1 : 0)},
                       {"allow", Datum::list(std::move(allow))},
                       {"anyHost", num(p.anyHost() ? 1 : 0)},
                       {"mailTo", Datum::list(std::move(mailTo))},
                       {"anyMail", num(p.anyMail() ? 1 : 0)},
                       {"secrets", std::move(secrets)},
                       {"webhooks", Datum::list(std::move(webhooks))},
                       {"warnings", Datum::list(std::move(warnings))}});
}

std::optional<HttpResponse> DevTools::reserved(const HttpRequest& request, const AdmittedRequest& admitted) {
    const std::string& path = admitted.path.path;
    if ((path == "/__dev/migrations/redo" || path == "/__dev/migrations/reset") && request.method == "POST") {
        // Only from the panel itself, as for /__dev/limits/reset.
        const std::string* origin = request.headers.get("Origin");
        const std::string* host = request.headers.get("Host");
        if (!origin || !host || *origin != "http://" + *host) return HttpResponse::text(403, "Forbidden\n");
        std::string message;
        if (path == "/__dev/migrations/redo" && migrations_.redoLast) message = migrations_.redoLast();
        if (path == "/__dev/migrations/reset" && migrations_.resetData) message = migrations_.resetData();
        {
            std::lock_guard<std::mutex> lock(messageMutex_);
            migrationMessage_ = message;
        }
        HttpResponse r;
        r.status = 303;
        r.headers.set("Location", "/__dev/migrations");
        return r;
    }
    if (path == "/__dev/migrations" && (request.method == "GET" || request.method == "HEAD")) {
        HttpResponse r;
        r.headers.set("Cache-Control", "no-store");
        r.headers.set("Content-Type", "text/html; charset=utf-8");
        std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Migrations</title><style>";
        html += kReportStyle;
        html += "</style></head>\n<body><main>\n<h1>Migrations</h1>\n<p class=\"note\">code/migrations/, and "
                "which of them this app's data has. In dev mode they're applied after each recompile, with a "
                "backup first. Dev mode only.</p>\n";
        std::string message;
        {
            std::lock_guard<std::mutex> lock(messageMutex_);
            message.swap(migrationMessage_);
        }
        if (!message.empty()) html += "<p class=\"done\">" + escapeHtml(message) + "</p>\n";
        Datum status = migrations_.status ? migrations_.status() : Datum::map({});
        const Datum::MapItems& st = status.kind() == Datum::Kind::Map ? status.asMap() : Datum::MapItems{};
        if (auto it = st.find("problems"); it != st.end() && it->second.kind() == Datum::Kind::List) {
            for (const Datum& p : it->second.asList()) {
                html += "<p class=\"warning\">" + escapeHtml(p.asString()) + "</p>\n";
            }
        }
        if (auto it = st.find("lastError"); it != st.end() && it->second.kind() == Datum::Kind::String &&
                                              !it->second.asString().empty()) {
            html += "<p class=\"warning\">Last failure: " + escapeHtml(it->second.asString()) + "</p>\n";
        }
        html += "<table>\n<tr><th>#</th><th>Migration</th><th>State</th><th>Applied</th></tr>\n";
        if (auto it = st.find("items"); it != st.end() && it->second.kind() == Datum::Kind::List) {
            for (const Datum& item : it->second.asList()) {
                const auto& m = item.asMap();
                std::string state = m.at("state").asString();
                html += "<tr" + std::string(state == "applied" ? "" : " class=\"restricted\"") + "><td>" +
                        std::to_string(static_cast<int>(m.at("number").asNumber())) + "</td><td>" +
                        escapeHtml(m.at("file").asString()) + "</td><td>" + escapeHtml(state) + "</td><td>" +
                        escapeHtml(m.at("appliedAt").asString()) + "</td></tr>\n";
            }
        }
        html += "</table>\n"
                "<form method=\"post\" action=\"/__dev/migrations/redo\"><button>Redo the last migration</button> "
                "<span class=\"note\">Goes back to the backup taken before the last migrations, and applies "
                "them again as they are now: for editing a migration until it's right.</span></form>\n"
                "<form method=\"post\" action=\"/__dev/migrations/reset\"><button>Reset dev data</button> "
                "<span class=\"note\">Empties data/ and the database (after a backup), runs every migration, "
                "then code/seed.ms.</span></form>\n</main></body></html>\n";
        r.body = std::move(html);
        return r;
    }
    if (path == "/__dev/limits/reset" && request.method == "POST") {
        // Only from the dev tools' own page: a page on another site can't
        // reset the counters (the dev server answers only to localhost, but
        // a browser would still send another site's form here).
        const std::string* origin = request.headers.get("Origin");
        const std::string* host = request.headers.get("Host");
        if (!origin || !host || *origin != "http://" + *host) return HttpResponse::text(403, "Forbidden\n");
        if (rates_) (void)rates_->resetCounters();
        {
            std::lock_guard<std::mutex> lock(hub_->mutex);
            hub_->recentLimits.push_back("(counters reset)");
        }
        HttpResponse r;
        r.status = 303;
        r.headers.set("Location", "/__dev/limits");
        return r;
    }
    if (request.method != "GET" && request.method != "HEAD") return std::nullopt;
    if (path == "/__dev/limits") {
        HttpResponse r;
        r.headers.set("Cache-Control", "no-store");
        r.headers.set("Content-Type", "text/html; charset=utf-8");
        std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Rate limits</title><style>";
        html += kReportStyle;
        html += "</style></head>\n<body><main>\n<h1>Rate limits</h1>\n<p class=\"note\">The counters in "
                "private/state/state.db, each limit's current count (a sliding window), and the latest requests "
                "counted or refused. Each route's limits are in the <a href=\"/__dev/access\">access report</a>. "
                "Dev mode only.</p>\n"
                "<form method=\"post\" action=\"/__dev/limits/reset\"><button>Reset all counters</button></form>\n"
                "<h2>Counters</h2>\n<table>\n<tr><th>Key</th><th>Window</th><th>Count</th></tr>\n";
        if (rates_) {
            if (auto counters = rates_->counters()) {
                for (const RateStore::Counter& c : counters.value()) {
                    char n[32];
                    std::snprintf(n, sizeof n, "%.1f", c.count);
                    html += "<tr><td>" + escapeHtml(c.key) + "</td><td>" + std::to_string(c.seconds) + " s</td><td>" +
                            n + "</td></tr>\n";
                }
            }
        }
        html += "</table>\n<h2>Latest</h2>\n<ul>\n";
        std::vector<std::string> recent;
        {
            std::lock_guard<std::mutex> lock(hub_->mutex);
            recent = hub_->recentLimits;
        }
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) html += "<li>" + escapeHtml(*it) + "</li>\n";
        html += "</ul>\n</main></body></html>\n";
        r.body = std::move(html);
        return r;
    }
    if (path == "/__dev/outbound" || path == "/__dev/outbound.json") {
        std::shared_ptr<const CompiledApp> app = currentApp_ ? currentApp_() : nullptr;
        if (!app) return HttpResponse::text(503, "The app hasn't compiled yet.\n");
        Datum report = outboundReport(*app, secrets_ ? secrets_() : Datum::list({}));
        std::vector<std::string> recent;
        {
            std::lock_guard<std::mutex> lock(hub_->mutex);
            recent = hub_->recentOutbound;
        }
        HttpResponse r;
        r.headers.set("Cache-Control", "no-store");
        if (path == "/__dev/outbound.json") {
            Datum::MapItems items = report.asMap();
            Datum::ListItems calls;
            for (const std::string& line : recent) calls.push_back(str(line));
            items["recent"] = Datum::list(std::move(calls));
            r.headers.set("Content-Type", "application/json");
            r.body = toJson(Datum::map(std::move(items)));
            return r;
        }
        const auto& m = report.asMap();
        auto list = [](const Datum& items) {
            std::string out;
            for (const Datum& d : items.asList()) out += "<li><code>" + escapeHtml(d.asString()) + "</code></li>\n";
            return out.empty() ? std::string("<li>(none)</li>\n") : out;
        };
        r.headers.set("Content-Type", "text/html; charset=utf-8");
        std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Outbound report</title><style>";
        html += kReportStyle;
        html += "</style></head>\n<body><main>\n<h1>Outbound report</h1>\n<p class=\"note\">Where app code may send "
                "requests and mail (code/outbound.ms, generation " + std::to_string(app->generation) + "), which "
                "secrets may go where (private/config/secrets.ms), the webhooks that take posts without a CSRF token, "
                "and the latest requests. Private and internal addresses are always refused, unless "
                "<code>config.outbound.allowAddresses</code> names them. Dev mode only.</p>\n";
        for (const Datum& w : m.at("warnings").asList()) {
            html += "<p class=\"warning\">" + escapeHtml(w.asString()) + "</p>\n";
        }
        if (m.at("declared").asNumber() == 0) {
            html += "<p class=\"warning\">There's no code/outbound.ms, so app code can't send requests, and can "
                    "mail only its users.</p>\n";
        }
        if (m.at("anyHost").asNumber() == 1) {
            html += "<p class=\"warning\">allow has \"*\": app code may reach any public host.</p>\n";
        }
        if (m.at("anyMail").asNumber() == 1) {
            html += "<p class=\"warning\">mailTo has \"*\": app code may mail any address.</p>\n";
        }
        html += "<h2>Hosts (allow)</h2>\n<ul>\n" + list(m.at("allow")) + "</ul>\n";
        html += "<h2>Mail to (mailTo), besides users</h2>\n<ul>\n" + list(m.at("mailTo")) + "</ul>\n";
        html += "<h2>Secrets</h2>\n<table>\n<tr><th>Name</th><th>Use</th><th>Hosts</th></tr>\n";
        for (const Datum& d : m.at("secrets").asList()) {
            const auto& sm = d.asMap();
            std::string hosts;
            for (const Datum& h : sm.at("hosts").asList()) hosts += (hosts.empty() ? "" : ", ") + escapeHtml(h.asString());
            html += "<tr><td>" + escapeHtml(sm.at("name").asString()) + "</td><td>" + escapeHtml(sm.at("use").asString()) +
                    "</td><td>" + hosts + "</td></tr>\n";
        }
        html += "</table>\n<h2>Webhooks</h2>\n<table>\n<tr><th>URL</th><th>Handler</th><th>Access</th></tr>\n";
        for (const Datum& d : m.at("webhooks").asList()) {
            const auto& wm = d.asMap();
            html += "<tr><td>POST " + escapeHtml(wm.at("url").asString()) + "</td><td>" +
                    escapeHtml(wm.at("handler").asString()) + "</td><td>" + escapeHtml(wm.at("access").asString()) +
                    "</td></tr>\n";
        }
        html += "</table>\n<h2>Latest requests</h2>\n<ul>\n";
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) html += "<li>" + escapeHtml(*it) + "</li>\n";
        html += "</ul>\n</main></body></html>\n";
        r.body = std::move(html);
        return r;
    }
    if (path == "/__dev/access" || path == "/__dev/access.json") {
        std::shared_ptr<const CompiledApp> app = currentApp_ ? currentApp_() : nullptr;
        if (!app) return HttpResponse::text(503, "The app hasn't compiled yet.\n");
        Datum report = accessReport(*app);
        HttpResponse r;
        r.headers.set("Cache-Control", "no-store");
        if (path == "/__dev/access.json") {
            r.headers.set("Content-Type", "application/json");
            r.body = toJson(report);
            return r;
        }
        r.headers.set("Content-Type", "text/html; charset=utf-8");
        std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Access report</title><style>";
        html += kReportStyle;
        html += "</style></head>\n<body><main>\n<h1>Access report</h1>\n<p class=\"note\">Every URL the app serves "
                "(generation " + std::to_string(app->generation) + "), who may use it, and the rules that say so: "
                "code/access.ms and each page file's <code>access =</code>. Dev mode only.</p>\n";
        for (const Datum& w : report.asMap().at("warnings").asList()) {
            html += "<p class=\"warning\">" + escapeHtml(w.asString()) + "</p>\n";
        }
        html += "<p class=\"note\">Every request but GET is also counted against the engine's default limits "
                "(<code>config.limits</code>). <a href=\"/__dev/limits\">Counters</a></p>\n";
        html += "<table>\n<tr><th>URL</th><th>Kind</th><th>Access</th><th>From</th><th>Limits</th><th>Form</th></tr>\n";
        for (const Datum& route : report.asMap().at("routes").asList()) {
            const auto& m = route.asMap();
            std::string access = m.at("access").asString();
            std::string sources, limits;
            for (const Datum& s : m.at("sources").asList()) sources += (sources.empty() ? "" : "<br>") + escapeHtml(s.asString());
            for (const Datum& s : m.at("limits").asList()) limits += (limits.empty() ? "" : "<br>") + escapeHtml(s.asString());
            html += "<tr" + std::string(access == "public" ? "" : " class=\"restricted\"") + "><td>" +
                    escapeHtml(m.at("url").asString()) + "</td><td>" + escapeHtml(m.at("kind").asString()) +
                    "</td><td>" + escapeHtml(access) + "</td><td>" + sources + "</td><td>" + limits + "</td><td>" +
                    escapeHtml(m.at("form").asString()) + "</td></tr>\n";
        }
        html += "</table>\n</main></body></html>\n";
        r.body = std::move(html);
        return r;
    }
    if (path == "/__dev/client.js") {
        HttpResponse r;
        r.headers.set("Content-Type", "text/javascript; charset=utf-8");
        r.body = kDevClientScript;
        return r;
    }
    if (path == "/__dev/reload") {
        HttpResponse r;
        r.headers.set("Content-Type", "text/event-stream");
        r.headers.set("Cache-Control", "no-store");
        std::shared_ptr<Hub> hub = hub_;
        r.stream = [hub](std::shared_ptr<StreamWriter> writer) { hub->add(std::move(writer)); };
        return r;
    }
    return std::nullopt;
}

void DevTools::renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) {
    std::string event;
    {
        std::lock_guard<std::mutex> lock(hub_->mutex);
        if (generation != hub_->warningsGeneration) {
            if (generation < hub_->warningsGeneration) return;   // from a request on an old version
            hub_->warningsGeneration = generation;
            hub_->warnings.clear();
        }
        bool added = false;
        for (const RenderWarning& w : warnings) {
            if (hub_->warnings.size() >= kMaxWarnings) break;
            if (std::find(hub_->warnings.begin(), hub_->warnings.end(), w) != hub_->warnings.end()) continue;
            hub_->warnings.push_back(w);
            added = true;
        }
        if (!added) return;
        event = serverSentEvent("warnings", Datum::map({{"generation", num(generation)},
                                                        {"warnings", hub_->warningList()}}));
    }
    std::shared_ptr<Hub> hub = hub_;
    hub->loop.post([hub, event] { hub->broadcast(event); });
}

void DevTools::decorate(HttpResponse& response, uint64_t generation, const std::string& nonce) {
    std::string tag = "<script src=\"/__dev/client.js\"";
    if (!nonce.empty()) tag += " nonce=\"" + escapeHtml(nonce) + "\"";
    tag += " data-generation=\"" + std::to_string(generation) + "\" defer></script>";
    insertBeforeBodyEnd(response.body, tag);
}

HttpResponse DevTools::scriptErrorPage(const ScriptError& error) {
    std::string where = error.file.empty() ? "a script" : error.file;
    if (error.line > 0) where += ", line " + std::to_string(error.line);

    std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Error</title><style>";
    html += kErrorPageStyle;
    html += "</style></head>\n<body><main>\n<h1>Script error</h1>\n<p class=\"where\">" + escapeHtml(where) +
            "</p>\n<p class=\"message\">" + escapeHtml(error.message) + "</p>\n";
    if (auto source = readCodeFile(code_, error.file)) {
        Excerpt e = excerptOf(*source, error.line);
        if (e.first > 0) {
            html += "<pre>";
            for (size_t i = 0; i < e.lines.size(); i++) {
                int n = e.first + static_cast<int>(i);
                html += n == error.line ? "<span class=\"hit\">" : "<span>";
                html += "<i>" + std::to_string(n) + "</i>" + escapeHtml(e.lines[i]) + "</span>";
            }
            html += "</pre>\n";
        }
    }
    html += "<p class=\"note\">Shown because this is dev mode. In production, visitors see the 500 page.</p>\n"
            "</main></body></html>\n";

    HttpResponse r;
    r.status = 500;
    r.headers.set("Content-Type", "text/html; charset=utf-8");
    r.body = std::move(html);
    return r;
}

}  // namespace fourfold
