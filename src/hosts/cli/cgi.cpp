#include "cgi.h"

#include <chrono>
#include <future>
#include <iterator>
#include <ostream>
#include <sstream>

#include "appdata/data_files.h"
#include "compiler/compiler.h"
#include "partition/app_folder.h"
#include "pipeline/engine.h"
#include "script/script_host.h"
#include "state/engine_log.h"
#include "state/lock_service.h"
#include "state/session_store.h"

namespace fourfold::cli {

namespace {

constexpr size_t kMaxBody = 1024 * 1024;

using SteadyClock = std::chrono::steady_clock;

double msSince(SteadyClock::time_point t) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - t).count();
}

// HTTP_USER_AGENT → User-Agent.
std::string headerName(const std::string& variable) {
    std::string name;
    bool upper = true;
    for (size_t i = 5; i < variable.size(); i++) {
        char c = variable[i];
        if (c == '_') {
            name += '-';
            upper = true;
        } else {
            name += upper ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            upper = false;
        }
    }
    return name;
}

// The request, as the CGI environment describes it.
HttpRequest requestFrom(const CgiEnvironment& env, std::istream& in) {
    HttpRequest r;
    r.method = env("REQUEST_METHOD").value_or("GET");
    if (auto uri = env("REQUEST_URI")) {
        r.target = *uri;
    } else {
        r.target = env("PATH_INFO").value_or("/");
        if (r.target.empty()) r.target = "/";
        std::string query = env("QUERY_STRING").value_or("");
        if (!query.empty()) r.target += "?" + query;
    }
    r.peerAddress = env("REMOTE_ADDR").value_or("");
    // The headers CGI passes as HTTP_* variables, plus the two it doesn't.
    static const char* const kHeaders[] = {"HTTP_HOST",          "HTTP_COOKIE", "HTTP_ORIGIN", "HTTP_REFERER",
                                           "HTTP_USER_AGENT",    "HTTP_ACCEPT", "HTTP_HX_REQUEST",
                                           "HTTP_X_CSRF_TOKEN",  "HTTP_ACCEPT_LANGUAGE"};
    for (const char* variable : kHeaders) {
        if (auto value = env(variable)) r.headers.add(headerName(variable), *value);
    }
    if (auto type = env("CONTENT_TYPE")) r.headers.add("Content-Type", *type);
    size_t length = 0;
    if (auto len = env("CONTENT_LENGTH")) length = static_cast<size_t>(std::strtoull(len->c_str(), nullptr, 10));
    if (length > 0 && length <= kMaxBody) {
        r.body.resize(length);
        in.read(r.body.data(), static_cast<std::streamsize>(length));
        r.body.resize(static_cast<size_t>(in.gcount()));
    }
    return r;
}

void writeResponse(const HttpResponse& response, std::ostream& out) {
    out << "Status: " << response.status << " " << statusReason(response.status) << "\r\n";
    for (const auto& field : response.headers) out << field.name << ": " << field.value << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n\r\n" << response.body;
    out.flush();
}

}  // namespace

int runCgi(const std::string& folder, const CgiEnvironment& env, std::istream& in, std::ostream& out,
           std::ostream& err, bool timing) {
    auto start = SteadyClock::now();
    auto fail = [&](const std::string& message) {
        err << "fourfold cgi: " << message << "\n";
        writeResponse(HttpResponse::text(500, "Internal Server Error\n"), out);
        return 1;
    };

    auto app = AppFolder::open(folder, AppFolder::Missing::Fail);
    if (!app) return fail(app.error().message);
    auto code = app.value().codeTree();
    auto engineCode = app.value().codeTree();
    auto state = app.value().stateDir();
    auto data = app.value().dataDir();
    auto dropbox = app.value().dropbox();
    if (!code || !engineCode || !state || !data || !dropbox) return fail("can't open the app's partitions");
    auto key = loadOrCreateEngineKey(state.value());
    auto dbPath = state.value().sqlitePath(RelPath::parse("state.db").value());
    if (!key || !dbPath) return fail((!key ? key.error() : dbPath.error()).message);
    auto sessions = SessionStore::open(dbPath.value(), SessionConfig{});
    if (!sessions) return fail("can't open private/state/state.db: " + sessions.error().message);

    auto scripts = ScriptHost::create();
    if (!scripts) return fail(scripts.error().message);
    // Session locks are shared with every other process serving this app.
    const StateDir& stateDir = state.value();
    auto sessionLocks = makeFileLockService([&stateDir](const std::string& key) { return stateDir.lockFile(key); },
                                            scripts.value()->workers());
    DataFiles dataFiles(std::move(data).value());
    EngineLog log(app.value().stateDir().value());
    double openMs = msSince(start);

    // Cold: compile every time. (M10 caches what can be cached.)
    auto compileStart = SteadyClock::now();
    CompileOptions compileOptions;
    compileOptions.scripts = scripts.value().get();
    CompileOutput compiled = compile(code.value(), compileOptions);
    double compileMs = msSince(compileStart);
    if (!compiled.ok()) {
        for (const auto& d : compiled.diagnostics) log.write("compile", d.toString());
        return fail("the app doesn't compile; see private/state/logs/");
    }

    EngineOptions options;   // any Host: Apache has already chosen this app
    options.sessions = sessions.value().get();
    options.sessionLocks = sessionLocks.get();
    options.data = &dataFiles;
    options.dropbox = &dropbox.value();
    options.log = &log;
    options.https = env("HTTPS").value_or("") == "on";
    options.engineKey = key.value();
    Engine engine(compiled.app, std::move(engineCode).value(), scripts.value().get(), std::move(options));

    auto handleStart = SteadyClock::now();
    std::promise<HttpResponse> done;
    engine.handle(requestFrom(env, in), [&done](HttpResponse r) { done.set_value(std::move(r)); });
    HttpResponse response = done.get_future().get();
    double handleMs = msSince(handleStart);
    if (response.stream) response = HttpResponse::text(501, "Server-sent events aren't available in CGI mode\n");
    writeResponse(response, out);

    if (timing) {
        err << "fourfold cgi: open " << openMs << " ms, compile " << compileMs << " ms, handle " << handleMs
            << " ms, total " << msSince(start) << " ms\n";
    }
    return 0;
}

}  // namespace fourfold::cli
