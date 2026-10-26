#include "devtools/dev_tools.h"

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

std::optional<HttpResponse> DevTools::reserved(const HttpRequest& request, const AdmittedRequest& admitted) {
    if (request.method != "GET" && request.method != "HEAD") return std::nullopt;
    const std::string& path = admitted.path.path;
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
