#include "pipeline/engine.h"

#include <cctype>

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

const char* const kSignInPage =
    "<!DOCTYPE html>\n<html><head><title>Sign in required</title></head>\n"
    "<body><h1>Sign in required</h1>\n"
    "<p>This page is for signed-in visitors. <a href=\"/__auth/login\">Sign in</a></p></body></html>\n";

const char* const kNotAllowedPage =
    "<!DOCTYPE html>\n<html><head><title>Not allowed</title></head>\n"
    "<body><h1>Not allowed</h1>\n"
    "<p>Your account doesn't have access to this page.</p></body></html>\n";

// How long the CSRF cookie lasts: a form can be submitted this long after
// its page was loaded.
constexpr int64_t kCsrfCookieSeconds = 400 * 24 * 3600;   // browsers' cap

bool isHtml(const HttpResponse& response) {
    const std::string* type = response.headers.get("Content-Type");
    return type && type->compare(0, 9, "text/html") == 0;
}

bool hasPrefix(const std::string& path, const char* prefix) {
    std::string p = prefix;
    return path == p || path.compare(0, p.size() + 1, p + "/") == 0;
}

bool isSafeMethod(const std::string& method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

// Bytes of randomness in each response's CSP nonce.
constexpr size_t kNonceBytes = 16;

// A query parameter's value, percent-encoded.
std::string encodeQueryValue(const std::string& s) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

UserFacts userFacts(const AuthUser& u) { return {u.id, u.name, u.email, u.roles}; }

// The `users` module's view of the AuthStore.
class StoreUsers : public UserDirectory {
public:
    explicit StoreUsers(AuthStore& store) : store_(store) {}

    Result<std::optional<UserFacts>> get(const std::string& id) override { return wrap(store_.get(id)); }
    Result<std::optional<UserFacts>> findByEmail(const std::string& email) override {
        return wrap(store_.findByEmail(email));
    }
    Status grant(const std::string& id, const std::string& role) override { return store_.grant(id, role); }
    Status revoke(const std::string& id, const std::string& role) override { return store_.revoke(id, role); }

private:
    static Result<std::optional<UserFacts>> wrap(Result<std::optional<AuthUser>> u) {
        if (!u) return u.error();
        if (!u.value()) return std::optional<UserFacts>();
        return std::optional<UserFacts>(userFacts(*u.value()));
    }
    AuthStore& store_;
};

std::string engineKeyFrom(const EngineOptions& options) {
    return options.engineKey.empty() ? randomBytes(32) : options.engineKey;
}

HttpResponse page(int status, const char* html, const std::string& reason, const EngineOptions& options) {
    HttpResponse r;
    r.status = status;
    r.headers.set("Content-Type", "text/html; charset=utf-8");
    r.body = html;
    if (options.showErrorDetails && !reason.empty()) {
        std::string note = "<p>" + escapeHtml(reason) + "</p>\n<p>(This explanation appears in dev mode only.)</p>";
        r.body.insert(r.body.rfind("</body>"), note);
    }
    return r;
}

void logError(const EngineOptions& options, const ScriptError& e) {
    if (!options.log) return;
    std::string where = e.file.empty() ? "" : e.file + (e.line ? ":" + std::to_string(e.line) : "") + ": ";
    options.log->write("error", where + e.message);
}

}  // namespace

// One request past Admit, shared by the steps that finish it (some of which
// run later, on other threads).
struct Engine::Flow {
    HttpRequest request;
    AdmittedRequest admitted;
    std::shared_ptr<const CompiledApp> app;
    std::shared_ptr<const AppConfig> config;
    std::shared_ptr<const EngineOptions> options;
    RenderOptions render;
    // A new CSRF cookie, to set on an HTML response: the visitor had none
    // (or a malformed one), and the page's forms carry its token.
    std::optional<std::string> newCsrfCookie;
    std::optional<std::string> csrfCookie;   // the visitor's, if well formed
    std::optional<std::string> sessionHash;
    std::optional<AuthUser> user;            // set by identify()
    RouteDecision decision;
    // The response depends on who's asking: shared caches mustn't keep it.
    bool personal = false;
    Done done;

    // The standard headers, a new CSRF cookie, cache rules, and the dev
    // tools' hook for full pages; then the response goes.
    void send(HttpResponse response, bool fullPage = true) const {
        addStandardHeaders(response, render.nonce);
        if (personal) response.headers.set("Cache-Control", "private, no-store");
        if (newCsrfCookie && isHtml(response)) {
            response.headers.add("Set-Cookie", setCookie("csrf", *newCsrfCookie, options->https, kCsrfCookieSeconds));
        }
        uint64_t generation = app ? app->generation : 0;
        if (fullPage && options->hooks && isHtml(response)) options->hooks->decorate(response, generation, render.nonce);
        done(std::move(response));
    }
};

Engine::Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options)
    : gate_(engineKeyFrom(options)),
      app_(std::move(app)),
      config_(std::make_shared<const AppConfig>()),
      users_(options.auth ? std::make_unique<StoreUsers>(*options.auth) : nullptr),
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

void Engine::setConfig(std::shared_ptr<const AppConfig> config) {
    std::lock_guard<std::mutex> lock(appMutex_);
    config_ = std::move(config);
}

std::shared_ptr<const AppConfig> Engine::config() const {
    std::lock_guard<std::mutex> lock(appMutex_);
    return config_;
}

void Engine::handle(const HttpRequest& request, Done done) const {
    auto f = std::make_shared<Flow>();
    f->options = options_;
    f->done = std::move(done);
    // One nonce per response (overview §8): every <script> the engine
    // renders carries it, and the CSP allows no other inline script.
    f->render.nonce = randomBase64(kNonceBytes);
    const EngineOptions& options = *options_;

    if (auto rejected = admit(request, options, f->admitted)) {
        f->send(std::move(*rejected), false);
        return;
    }
    const std::string& path = f->admitted.path.path;

    // Reserved paths go to their owners before routing (Arch §5, pipeline).
    if (hasPrefix(path, "/__dev")) {
        std::optional<HttpResponse> response;
        if (options.hooks) response = options.hooks->reserved(request, f->admitted);
        f->send(response ? std::move(*response) : HttpResponse::text(404, "Not Found\n"), false);
        return;
    }
    if (hasPrefix(path, "/__fourfold")) {
        std::optional<HttpResponse> response;
        if (request.method == "GET" || request.method == "HEAD") response = builtinAsset(path);
        f->send(response ? std::move(*response) : HttpResponse::text(404, "Not Found\n"), false);
        return;
    }

    // Pin this generation of the app, and the config, for the whole request.
    f->app = this->app();
    f->config = this->config();
    f->request = request;
    bool auth = hasPrefix(path, "/__auth");
    if (!f->app && !auth) {
        HttpResponse response;
        response.status = 503;
        response.headers.set("Content-Type", "text/html; charset=utf-8");
        response.headers.set("Retry-After", "1");
        response.body = kNotCompiledPage;
        f->send(std::move(response));
        return;
    }
    std::string nonce = f->render.nonce;
    if (f->app) {
        f->render = renderOptionsFor(*f->app, nonce);
    } else {
        f->render = RenderOptions{};
        f->render.nonce = nonce;
    }

    // The visitor's cookies. Every page the engine renders carries the CSRF
    // token of the visitor's CSRF cookie, made now if missing.
    f->csrfCookie = cookieValue(request, cookieName("csrf", options.https));
    if (!f->csrfCookie || !PolicyGate::isCsrfCookie(*f->csrfCookie)) {
        f->csrfCookie.reset();
        f->newCsrfCookie = PolicyGate::newCsrfCookie();
    }
    f->render.csrfToken = gate_.tokenFor(f->csrfCookie ? *f->csrfCookie : *f->newCsrfCookie);
    if (auto id = cookieValue(request, cookieName("session", options.https))) {
        f->sessionHash = SessionStore::hashOfId(*id);
    }

    if (auth) {
        identify(f, [this](std::shared_ptr<Flow> flow) { serveAuth(std::move(flow)); });
        return;
    }
    f->decision = route(f->admitted, *f->app);
    const RouteEntry* entry = f->decision.entry;
    // Identify: who's asking, if it matters (the route is restricted, or runs
    // code that sees `req.user`).
    if (entry && (!entry->access.isPublic() || f->decision.handler)) {
        identify(f, [this](std::shared_ptr<Flow> flow) { serveRoute(std::move(flow)); });
    } else {
        serveRoute(std::move(f));
    }
}

void Engine::identify(std::shared_ptr<Flow> f, std::function<void(std::shared_ptr<Flow>)> next) const {
    const EngineOptions& options = *options_;
    if (!f->sessionHash || !options.sessions || !options.auth || !scripts_) {
        next(std::move(f));
        return;
    }
    // Both lookups are SQLite, so on a worker. Roles are read on every
    // request, never cached in the session, so a change applies at once.
    scripts_->workers().submit([f, next] {
        const EngineOptions& options = *f->options;
        auto record = options.sessions->load(*f->sessionHash);
        if (!record) {
            if (options.log) options.log->write("session", "looking up a session failed: " + record.error().message);
        } else if (record.value() && record.value()->userId) {
            auto user = options.auth->get(*record.value()->userId);
            if (!user) {
                if (options.log) options.log->write("auth", "looking up a user failed: " + user.error().message);
            } else if (user.value()) {
                f->user = std::move(*user.value());
            }
        }
        next(f);
    });
}

void Engine::serveAuth(std::shared_ptr<Flow> f) const {
    const EngineOptions& options = *options_;
    f->personal = true;
    if (!options.authRoutes) {
        f->send(HttpResponse::text(404, "Not Found\n"), false);
        return;
    }
    std::optional<std::string> csrfToken;
    RequestFacts facts = requestFacts(f->request, f->admitted, &csrfToken);
    const std::string* host = f->request.headers.get("Host");
    std::string origin = std::string(options.https ? "https://" : "http://") + (host ? *host : "");
    if (!isSafeMethod(f->request.method)) {
        // The sign-in forms are public, but get the same CSRF protection as
        // every other form: a site can't sign a visitor in or out.
        static const RouteEntry kPublic{RouteKind::Page, RelPath()};
        GateFacts gateFacts;
        gateFacts.method = f->request.method;
        if (const std::string* o = f->request.headers.get("Origin")) gateFacts.origin = *o;
        gateFacts.ownOrigin = origin;
        gateFacts.csrfCookie = f->csrfCookie;
        gateFacts.csrfToken = csrfToken;
        GateDecision gate = gate_.check(kPublic, gateFacts);
        if (!gate.approval) {
            if (options.log) options.log->write("csrf", f->request.method + " " + f->admitted.path.path + ": " + gate.reason);
            f->send(page(403, kForbiddenPage, gate.reason, options));
            return;
        }
    }
    AuthRequest a;
    a.method = f->request.method;
    a.path = f->admitted.path.path;
    a.query = std::move(facts.query);
    a.form = std::move(facts.form);
    a.sessionHash = f->sessionHash;
    a.addressGroup = addressGroup(f->request.peerAddress);
    a.user = f->user;
    a.app = f->app;
    a.config = f->config;
    a.render = f->render;
    a.origin = origin;
    a.https = options.https;
    options.authRoutes->handle(std::move(a), [f](HttpResponse response) { f->send(std::move(response)); });
}

void Engine::serveRoute(std::shared_ptr<Flow> f) const {
    const EngineOptions& options = *options_;
    const CompiledApp& app = *f->app;
    const RouteDecision& decision = f->decision;
    const RouteEntry* entry = decision.entry;
    const HttpRequest& request = f->request;

    if (!entry) {   // not found, a redirect, or the wrong method
        f->send(dispatch(decision, app, code_, f->render));
        return;
    }

    // Gate: nothing is served without the policy's approval.
    f->personal = !entry->access.isPublic() || f->user.has_value();
    std::optional<std::string> csrfToken;
    RequestFacts facts = requestFacts(request, f->admitted, &csrfToken);
    GateFacts gateFacts;
    gateFacts.method = request.method;
    if (const std::string* origin = request.headers.get("Origin")) gateFacts.origin = *origin;
    const std::string* host = request.headers.get("Host");
    gateFacts.ownOrigin = std::string(options.https ? "https://" : "http://") + (host ? *host : "");
    gateFacts.csrfCookie = f->csrfCookie;
    gateFacts.csrfToken = csrfToken;
    if (f->user) gateFacts.visitor = {true, f->user->roles};
    GateDecision gate = gate_.check(*entry, gateFacts);
    if (!gate.approval) {
        const char* category = gate.denial == GateDecision::Denial::Csrf ? "csrf" : "access";
        if (options.log) options.log->write(category, request.method + " " + f->admitted.path.path + ": " + gate.reason);
        bool htmx = isHtmxRequest(request);
        if (gate.denial == GateDecision::Denial::NeedLogin && (request.method == "GET" || request.method == "HEAD")) {
            // Sign in, then come back here.
            std::string back = encodeUrlPath(f->admitted.path.path) + (f->admitted.path.trailingSlash ? "/" : "");
            if (f->admitted.hasQuery) back += "?" + f->admitted.query;
            std::string login = "/__auth/login?next=" + encodeQueryValue(back);
            HttpResponse r;
            if (htmx) {
                r.status = 401;
                r.headers.set("HX-Redirect", login);
            } else {
                r.status = 303;
                r.headers.set("Location", login);
            }
            f->send(std::move(r), false);
            return;
        }
        if (gate.denial == GateDecision::Denial::Csrf && options.observer) {
            options.observer->scriptError({"request refused: " + gate.reason, "", 0});
        }
        const char* html = gate.denial == GateDecision::Denial::NeedLogin ? kSignInPage
                           : gate.denial == GateDecision::Denial::Forbidden ? kNotAllowedPage
                                                                            : kForbiddenPage;
        f->send(page(gate.status, html, gate.reason, options));
        return;
    }

    bool useSession = entry->usesSession && options.sessions && options.sessionLocks;
    if (!decision.handler) {
        if (!useSession || !f->sessionHash || !scripts_) {
            f->send(dispatch(decision, app, code_, f->render));
            return;
        }
        // A page with no code but a {{flash}} field: show `session.flash`, if
        // any, and remove it. Without the session's lock: the removal happens
        // only if no one changed the session meanwhile.
        const CodeTree* code = &code_;   // not read for a page (see dispatch)
        scripts_->workers().submit([f, code] {
            SessionStore& store = *f->options->sessions;
            std::optional<SessionRecord> record;
            if (auto loaded = store.load(*f->sessionHash); loaded) record = loaded.value();
            Datum::MapItems items;
            if (record) {
                if (auto parsed = parseJson(record->app); parsed && parsed.value().kind() == Datum::Kind::Map) {
                    items = parsed.value().asMap();
                }
            }
            auto flash = items.find("flash");
            MapFields fields;
            fields.set("flash", flash != items.end() ? FieldValue::datum(flash->second) : FieldValue::null());
            HttpResponse response = dispatch(f->decision, *f->app, *code, f->render, &fields);
            if (flash != items.end() && response.status == 200) {
                items.erase(flash);
                (void)store.replaceApp(*f->sessionHash, record->app, toJson(Datum::map(std::move(items))));
            }
            f->send(std::move(response));
        });
        return;
    }

    if (!scripts_) {
        f->send(errorResponse({"no script host to run " + decision.handler->label(), "", 0}, app, options, f->render));
        return;
    }

    RunContext context;
    context.request = std::move(facts);
    if (f->user) context.request.user = userFacts(*f->user);
    context.render = f->render;
    context.data = options.data;
    context.dropbox = options.dropbox;
    context.users = users_.get();
    std::shared_ptr<EngineSession> session;
    if (useSession) {
        session = std::make_shared<EngineSession>(*options.sessions, *options.sessionLocks, scripts_->workers(),
                                                  f->sessionHash, addressGroup(request.peerAddress), options.https);
        context.session = session;
        context.sessionMaxBytes = options.sessions->config().maxBytes;
    }

    // The route entry lives in the app, which `f` keeps alive.
    bool htmx = isHtmxRequest(request);
    WorkerPool* workers = &scripts_->workers();
    scripts_->run(
        f->app, *decision.handler, *gate.approval, std::move(context),
        [f, entry, htmx, session, workers](HandlerOutcome outcome) {
            const EngineOptions& options = *f->options;
            if (options.observer && !outcome.warnings.empty()) {
                options.observer->renderWarnings(f->app->generation, outcome.warnings);
            }
            auto respondWith = [f, entry, htmx](HandlerOutcome outcome, std::optional<std::string> cookie) {
                const EngineOptions& options = *f->options;
                if (outcome.kind == HandlerOutcome::Kind::Error) logError(options, outcome.error);
                HttpResponse response = respond(outcome, *entry, *f->app, options, f->render, htmx);
                if (cookie) response.headers.add("Set-Cookie", *cookie);
                // A fragment goes into a page that already has the dev tools.
                bool fullPage = outcome.kind != HandlerOutcome::Kind::Fragment &&
                                outcome.kind != HandlerOutcome::Kind::Status;
                f->send(std::move(response), fullPage);
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
