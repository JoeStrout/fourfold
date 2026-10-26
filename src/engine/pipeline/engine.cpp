#include "pipeline/engine.h"

#include "platform/random.h"
#include "session.h"

namespace fourfold {

namespace {

const char* const kNotCompiledPage =
    "<!DOCTYPE html>\n<html><head><title>Not compiled yet</title></head>\n"
    "<body><h1>The app hasn't compiled yet</h1>\n"
    "<p>Fix the errors listed where Fourfold is running, and this page will load.</p></body></html>\n";

const char* const kForbiddenPage =
    "<!DOCTYPE html>\n<html><head><title>Forbidden</title></head>\n"
    "<body><h1>This form has expired</h1>\n"
    "<p>Go back, reload the page, and try again.</p></body></html>\n";

// How long the CSRF cookie lasts: a form can be submitted this long after
// its page was loaded.
constexpr int64_t kCsrfCookieSeconds = 400 * 24 * 3600;   // browsers' cap

bool isHtml(const HttpResponse& response) {
    const std::string* type = response.headers.get("Content-Type");
    return type && type->compare(0, 9, "text/html") == 0;
}

// "/__dev/reload" and "/__dev" are the dev tools'.
bool isDevPath(const std::string& path) {
    return path == "/__dev" || path.compare(0, 7, "/__dev/") == 0;
}

// "/__fourfold/htmx.js" and "/__fourfold" are the engine's own.
bool isEnginePath(const std::string& path) {
    return path == "/__fourfold" || path.compare(0, 12, "/__fourfold/") == 0;
}

// Bytes of randomness in each response's CSP nonce.
constexpr size_t kNonceBytes = 16;

// One request's state after routing, shared by the steps that finish it
// (some of which run later, on other threads).
struct Pending {
    std::shared_ptr<const CompiledApp> app;
    std::shared_ptr<const EngineOptions> options;
    RenderOptions render;
    // A new CSRF cookie, to set on an HTML response: the visitor had none
    // (or a malformed one), and the page's forms carry its token.
    std::optional<std::string> newCsrfCookie;
};

// The standard headers, a new CSRF cookie, and the dev tools' hook for full
// pages.
void finish(HttpResponse& response, const Pending& p, bool page = true) {
    addStandardHeaders(response, p.render.nonce);
    if (p.newCsrfCookie && isHtml(response)) {
        response.headers.add("Set-Cookie", setCookie("csrf", *p.newCsrfCookie, p.options->https, kCsrfCookieSeconds));
    }
    uint64_t generation = p.app ? p.app->generation : 0;
    if (page && p.options->hooks && isHtml(response)) p.options->hooks->decorate(response, generation, p.render.nonce);
}

std::string engineKeyFrom(const EngineOptions& options) {
    return options.engineKey.empty() ? randomBytes(32) : options.engineKey;
}

HttpResponse forbidden(const std::string& reason, const EngineOptions& options) {
    HttpResponse r;
    r.status = 403;
    r.headers.set("Content-Type", "text/html; charset=utf-8");
    r.body = kForbiddenPage;
    if (options.showErrorDetails) {
        r.body = "<!DOCTYPE html>\n<html><head><title>Forbidden</title></head>\n<body><h1>Request refused</h1>\n"
                 "<p>" + escapeHtml(reason) + "</p>\n<p>(This explanation appears in dev mode only.)</p></body></html>\n";
    }
    return r;
}

void logError(const EngineOptions& options, const ScriptError& e) {
    if (!options.log) return;
    std::string where = e.file.empty() ? "" : e.file + (e.line ? ":" + std::to_string(e.line) : "") + ": ";
    options.log->write("error", where + e.message);
}

}  // namespace

Engine::Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options)
    : gate_(engineKeyFrom(options)),
      app_(std::move(app)),
      code_(std::move(code)),
      scripts_(scripts),
      options_(std::make_shared<const EngineOptions>(std::move(options))) {}

void Engine::setApp(std::shared_ptr<const CompiledApp> app) {
    std::shared_ptr<const CompiledApp> old;
    {
        std::lock_guard<std::mutex> lock(appMutex_);
        old = std::move(app_);
        app_ = std::move(app);
    }
    // `old` is let go here, outside the lock; its bytecode goes when the
    // requests still using it finish.
}

std::shared_ptr<const CompiledApp> Engine::app() const {
    std::lock_guard<std::mutex> lock(appMutex_);
    return app_;
}

void Engine::handle(const HttpRequest& request, Done done) const {
    // One nonce per response (overview §8): every <script> the engine
    // renders carries it, and the CSP allows no other inline script.
    Pending p;
    p.options = options_;
    p.render.nonce = randomBase64(kNonceBytes);
    const EngineOptions& options = *options_;

    AdmittedRequest admitted;
    if (auto rejected = admit(request, options, admitted)) {
        addStandardHeaders(*rejected, p.render.nonce);
        done(std::move(*rejected));
        return;
    }

    // Reserved paths go to their owners before routing (Arch §5, pipeline).
    if (isDevPath(admitted.path.path)) {
        std::optional<HttpResponse> response;
        if (options.hooks) response = options.hooks->reserved(request, admitted);
        if (!response) response = HttpResponse::text(404, "Not Found\n");
        addStandardHeaders(*response, p.render.nonce);
        done(std::move(*response));
        return;
    }
    if (isEnginePath(admitted.path.path)) {
        std::optional<HttpResponse> response;
        if (request.method == "GET" || request.method == "HEAD") response = builtinAsset(admitted.path.path);
        if (!response) response = HttpResponse::text(404, "Not Found\n");
        addStandardHeaders(*response, p.render.nonce);
        done(std::move(*response));
        return;
    }

    // Pin this generation of the app for the whole request.
    p.app = this->app();
    if (!p.app) {
        HttpResponse response;
        response.status = 503;
        response.headers.set("Content-Type", "text/html; charset=utf-8");
        response.headers.set("Retry-After", "1");
        response.body = kNotCompiledPage;
        finish(response, p);
        done(std::move(response));
        return;
    }
    const CompiledApp& app = *p.app;
    RouteDecision decision = route(admitted, app);
    std::string nonce = p.render.nonce;
    p.render = renderOptionsFor(app, nonce);

    // Identify: the visitor's cookies. Every page the engine renders carries
    // the CSRF token of the visitor's CSRF cookie, made now if missing.
    std::optional<std::string> csrfCookie = cookieValue(request, cookieName("csrf", options.https));
    if (!csrfCookie || !PolicyGate::isCsrfCookie(*csrfCookie)) {
        csrfCookie.reset();
        p.newCsrfCookie = PolicyGate::newCsrfCookie();
    }
    p.render.csrfToken = gate_.tokenFor(csrfCookie ? *csrfCookie : *p.newCsrfCookie);
    std::optional<std::string> sessionHash;
    if (auto id = cookieValue(request, cookieName("session", options.https))) sessionHash = SessionStore::hashOfId(*id);
    const RouteEntry* entry = decision.entry;
    bool useSession = entry && entry->usesSession && options.sessions && options.sessionLocks;

    if (!decision.handler) {
        if (!useSession || !sessionHash || !scripts_) {
            HttpResponse response = dispatch(decision, app, code_, p.render);
            finish(response, p);
            done(std::move(response));
            return;
        }
        // A page with no code but a {{flash}} field: show the waiting flash
        // message, if any, and clear it. Without the session's lock: the
        // clear happens only if no one changed the session meanwhile.
        std::string hash = *sessionHash;
        const CodeTree* code = &code_;   // not read for a page (see dispatch)
        scripts_->workers().submit([p, decision, code, hash, done]() {
            SessionStore& store = *p.options->sessions;
            std::optional<SessionRecord> record;
            if (auto loaded = store.load(hash); loaded) record = loaded.value();
            MapFields fields;
            EnginePart engine = record ? EnginePart::parse(record->engine) : EnginePart{};
            fields.set("flash", engine.flash ? FieldValue::string(*engine.flash) : FieldValue::null());
            HttpResponse response = dispatch(decision, *p.app, *code, p.render, &fields);
            if (engine.flash && response.status == 200) {
                (void)store.replaceEngine(hash, record->engine, EnginePart{}.json());
            }
            finish(response, p);
            done(std::move(response));
        });
        return;
    }

    if (!scripts_) {
        HttpResponse response = errorResponse({"no script host to run " + decision.handler->label(), "", 0}, app,
                                              options, p.render);
        finish(response, p);
        done(std::move(response));
        return;
    }

    // Gate: nothing reaches a handler without the policy's approval.
    std::optional<std::string> csrfToken;
    RequestFacts facts = requestFacts(request, admitted, &csrfToken);
    GateFacts gateFacts;
    gateFacts.method = request.method;
    if (const std::string* origin = request.headers.get("Origin")) gateFacts.origin = *origin;
    const std::string* host = request.headers.get("Host");
    gateFacts.ownOrigin = std::string(options.https ? "https://" : "http://") + (host ? *host : "");
    gateFacts.csrfCookie = csrfCookie;
    gateFacts.csrfToken = csrfToken;
    GateDecision gate = gate_.check(*entry, gateFacts);
    if (!gate.approval) {
        if (options.log) options.log->write("csrf", request.method + " " + admitted.path.path + ": " + gate.reason);
        if (options.observer) options.observer->scriptError({"request refused: " + gate.reason, "", 0});
        HttpResponse response = forbidden(gate.reason, options);
        finish(response, p);
        done(std::move(response));
        return;
    }

    RunContext context;
    context.request = std::move(facts);
    context.render = p.render;
    context.data = options.data;
    context.dropbox = options.dropbox;
    std::shared_ptr<EngineSession> session;
    if (useSession) {
        session = std::make_shared<EngineSession>(*options.sessions, *options.sessionLocks, scripts_->workers(),
                                                  sessionHash, addressGroup(request.peerAddress), options.https);
        context.session = session;
        context.sessionMaxBytes = options.sessions->config().maxBytes;
    }

    // The route entry lives in the app, which `p` keeps alive.
    bool htmx = isHtmxRequest(request);
    WorkerPool* workers = &scripts_->workers();
    scripts_->run(
        p.app, *decision.handler, *gate.approval, std::move(context),
        [p, entry, htmx, session, workers, done = std::move(done)](HandlerOutcome outcome) {
            const EngineOptions& options = *p.options;
            if (options.observer && !outcome.warnings.empty()) {
                options.observer->renderWarnings(p.app->generation, outcome.warnings);
            }
            auto respondWith = [p, entry, htmx, done](HandlerOutcome outcome, std::optional<std::string> cookie) {
                const EngineOptions& options = *p.options;
                if (outcome.kind == HandlerOutcome::Kind::Error) logError(options, outcome.error);
                HttpResponse response = respond(outcome, *entry, *p.app, options, p.render, htmx);
                if (cookie) response.headers.add("Set-Cookie", *cookie);
                // A fragment goes into a page that already has the dev tools.
                bool page = outcome.kind != HandlerOutcome::Kind::Fragment &&
                            outcome.kind != HandlerOutcome::Kind::Status;
                finish(response, p, page);
                done(std::move(response));
            };
            if (!session) {
                respondWith(std::move(outcome), std::nullopt);
                return;
            }
            // Commit: save what the run did to the session (SQLite, so on a
            // worker), before the response goes out.
            auto shared = std::make_shared<HandlerOutcome>(std::move(outcome));
            workers->submit([session, shared, respondWith] {
                EngineSession::Committed c = session->commit(shared->session);
                if (c.error) {
                    HandlerOutcome failed;
                    failed.kind = HandlerOutcome::Kind::Error;
                    failed.error.message = "the session couldn't be saved: " + c.error->message;
                    respondWith(std::move(failed), std::nullopt);
                    return;
                }
                respondWith(std::move(*shared), c.setCookie);
            });
        });
}

}  // namespace fourfold
