#include "pipeline/engine.h"

#include <algorithm>
#include <cctype>

#include "base/crypto.h"
#include "platform/random.h"
#include "session.h"

namespace fourfold {

namespace {

const char* const kNotCompiledPage =
    "<!DOCTYPE html>\n<html><head><title>Not compiled yet</title></head>\n"
    "<body><h1>The app hasn't compiled yet</h1>\n"
    "<p>Fix the errors listed where Fourfold is running, and this page will load.</p></body></html>\n";

const char* const kUnavailablePage =
    "<!DOCTYPE html>\n<html><head><title>Down for maintenance</title></head>\n"
    "<body><h1>Down for maintenance</h1>\n"
    "<p>The site is being updated, and will be back shortly. Please try again in a minute.</p></body></html>\n";

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

// ratelimit()'s view of the RateStore.
class StoreRates : public RateCounter {
public:
    explicit StoreRates(RateStore& store) : store_(store) {}
    bool hit(const std::string& key, const RateRule& rule) override {
        auto r = store_.hit({{key, rule.count, rule.seconds}});
        return !r || r.value().allowed;
    }

private:
    RateStore& store_;
};

// The `mail` module's sender (overview §6): finds a user's address, counts
// the app's hourly limit, and sends through the config's transport.
class EngineMail : public AppMailer {
public:
    EngineMail(std::shared_ptr<const AppConfig> config, AuthStore* auth, RateStore* rates, EngineLog* log)
        : config_(std::move(config)), auth_(auth), rates_(rates), log_(log) {}

    Result<std::string> send(const AppMailMessage& m) override {
        const AppConfig& c = *config_;
        if (!c.mail.appMail) return makeError(ErrorCode::Denied, "app mail is turned off (config.mail.appMail)");
        if (!c.mailer) {
            return makeError(ErrorCode::Denied, "no way to send mail is set up (config.mail.transport is \"none\")");
        }
        auto to = addressOf(m.to);
        if (!to) return to.error();
        std::vector<std::string> bcc;
        for (const AppMailRecipient& r : m.bcc) {
            auto address = addressOf(r);
            if (!address) return address.error();
            // A copy to the main recipient, or the same one twice, is dropped.
            if (address.value() != to.value() && std::find(bcc.begin(), bcc.end(), address.value()) == bcc.end()) {
                bcc.push_back(address.value());
            }
        }
        std::string all = to.value();
        for (const std::string& b : bcc) all += ", bcc " + b;
        if (rates_) {
            // Each recipient counts: all of them, or none.
            RateCheck check{"engine:mail|app", c.mail.appPerHour, 3600, static_cast<int64_t>(1 + bcc.size())};
            auto r = rates_->hit({check});
            if (r && !r.value().allowed) {
                if (log_) log_->write("mail", "app mail to " + all + " refused: over " +
                                                  std::to_string(c.mail.appPerHour) + " an hour");
                return makeError(ErrorCode::TooLarge, "over the app's limit of " + std::to_string(c.mail.appPerHour) +
                                                          " messages an hour (config.mail.appPerHour; each recipient "
                                                          "counts)");
            }
        }
        MailMessage message;
        message.from = c.mail.from;
        message.to = to.value();
        message.bcc = std::move(bcc);
        message.subject = m.subject;
        message.text = m.text;
        message.html = m.html;
        if (Status s = c.mailer->send(message); !s) {
            if (log_) log_->write("mail", "app mail to " + all + " failed: " + s.error().message);
            return s.error();
        }
        if (log_) log_->write("mail", "app mail sent to " + all + ": " + m.subject);
        return to.value();
    }

private:
    // A recipient's address: a user's, looked up, or the one given.
    Result<std::string> addressOf(const AppMailRecipient& r) {
        std::string address = r.address;
        if (!r.userId.empty()) {
            if (!auth_) return makeError(ErrorCode::Denied, "there are no users here to send mail to");
            auto user = auth_->get(r.userId);
            if (!user) return user.error();
            if (!user.value()) return makeError(ErrorCode::NotFound, "there's no user with the id " + r.userId);
            address = user.value()->email;
        }
        if (!isValidEmailAddress(address)) {
            return makeError(ErrorCode::Invalid, "\"" + address + "\" isn't an email address");
        }
        return address;
    }

    std::shared_ptr<const AppConfig> config_;
    AuthStore* auth_;
    RateStore* rates_;
    EngineLog* log_;
};

const char* const kTooManyPage =
    "<!DOCTYPE html>\n<html><head><title>Too many requests</title></head>\n"
    "<body><h1>Too many requests</h1>\n"
    "<p>Please wait a little, then try again.</p></body></html>\n";

const char* const kTooFastPage =
    "<!DOCTYPE html>\n<html><head><title>That was quick</title></head>\n"
    "<body><h1>That was quick!</h1>\n"
    "<p>Please wait a moment, then send the form again.</p></body></html>\n";

const char* const kNeedsJsPage =
    "<!DOCTYPE html>\n<html><head><title>This form needs JavaScript</title></head>\n"
    "<body><h1>This form needs JavaScript</h1>\n"
    "<p>This form needs JavaScript turned on to be sent. Go back, reload the page, and try again.</p></body></html>\n";

// Extra slack for a multipart body's boundaries and part headers.
constexpr size_t kMultipartOverhead = 64 * 1024;

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
    std::shared_ptr<OutboundClient> outbound;   // made with `config`
    std::shared_ptr<const EngineOptions> options;
    RenderOptions render;
    // A new CSRF cookie, to set on an HTML response: the visitor had none
    // (or a malformed one), and the page's forms carry its token.
    std::optional<std::string> newCsrfCookie;
    std::optional<std::string> csrfCookie;   // the visitor's, if well formed
    std::optional<std::string> sessionHash;
    std::optional<AuthUser> user;            // set by identify()
    // The visitor's address: the connection's, or a trusted proxy's
    // forwarded one (`req.ip`).
    std::string client;
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
      rates_(options.rates ? std::make_unique<StoreRates>(*options.rates) : nullptr),
      code_(std::move(code)),
      scripts_(scripts),
      options_(std::make_shared<const EngineOptions>(std::move(options))) {
    outbound_ = makeOutbound(*config_);
}

std::shared_ptr<OutboundClient> Engine::makeOutbound(const AppConfig& config) const {
    std::shared_ptr<const EngineOptions> options = options_;
    return std::make_shared<OutboundClient>(config.outbound, config.secrets, [options](const OutboundCall& call) {
        // Every call, with secrets and query strings left out (overview §6, Visibility).
        if (options->log) {
            std::string line = call.method + " " + call.url;
            if (call.refused.empty()) {
                line += " " + std::to_string(call.status) + " " + std::to_string(call.durationMs) + " ms " +
                        std::to_string(call.bytes) + " bytes";
                if (call.redirects) line += " (" + std::to_string(call.redirects) + " redirects)";
            } else {
                line += " refused: " + call.refused;
            }
            options->log->write("outbound", line);
        }
        if (options->observer) options->observer->outboundCall(call);
    });
}

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
    std::shared_ptr<OutboundClient> outbound = makeOutbound(*config);
    std::lock_guard<std::mutex> lock(appMutex_);
    config_ = std::move(config);
    outbound_ = std::move(outbound);
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

    f->request = request;
    // The schema lock (overview §3, Applying migrations): held shared for
    // the whole request. While migrations run, requests wait here, and
    // then go on to whichever version is current.
    if (SchemaGate* schema = options.schema) {
        Done inner = std::move(f->done);
        f->done = [schema, inner](HttpResponse response) {
            schema->leave();
            inner(std::move(response));
        };
        schema->enter([this, f, inner](bool admitted) {
            if (admitted) {
                proceed(f);
                return;
            }
            f->done = inner;   // never inside, so nothing to leave
            f->app = this->app();
            f->send(unavailable(f->app.get(), f->render.nonce), false);
        });
        return;
    }
    proceed(std::move(f));
}

HttpResponse Engine::unavailable(const CompiledApp* app, const std::string& nonce) const {
    HttpResponse r;
    if (app && app->unavailablePage) {
        if (const CompiledTemplate* t = app->templates.find(*app->unavailablePage)) {
            RenderOptions quiet = renderOptionsFor(*app, nonce);
            quiet.warnings = false;
            r.body = render(*t, EmptyFields(), quiet).html.str();
        }
    }
    if (r.body.empty()) r.body = kUnavailablePage;
    r.status = 503;
    r.headers.set("Content-Type", "text/html; charset=utf-8");
    r.headers.set("Retry-After", "30");
    r.headers.set("Cache-Control", "no-store");
    return r;
}

void Engine::proceed(std::shared_ptr<Flow> f) const {
    const EngineOptions& options = *options_;
    const HttpRequest& request = f->request;
    const std::string& path = f->admitted.path.path;
    // Pin this generation of the app, and the config, for the whole request.
    {
        std::lock_guard<std::mutex> lock(appMutex_);
        f->app = app_;
        f->config = config_;
        f->outbound = outbound_;
    }
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
    const std::string& cookie = f->csrfCookie ? *f->csrfCookie : *f->newCsrfCookie;
    f->render.csrfToken = gate_.tokenFor(cookie);
    // The timing token, and puzzles for forms whose routes ask for them
    // (overview §5), bound to the same cookie.
    int64_t now = options.clock->nowMillis();
    f->render.formTimeToken = gate_.formTimeToken(cookie, now);
    int bits = f->config->limits.challengeBits;
    const PolicyGate* gate = &gate_;
    f->render.newChallenge = [gate, cookie, now, bits] { return gate->newChallenge(cookie, now, bits); };
    f->client = clientAddress(request, f->config->proxy.trust);
    if (auto id = cookieValue(request, cookieName("session", options.https))) {
        f->sessionHash = SessionStore::hashOfId(*id);
    }

    if (auth) {
        identify(f, [this](std::shared_ptr<Flow> flow) { serveAuth(std::move(flow)); });
        return;
    }
    if (path.compare(0, std::char_traits<char>::length(kLivePrefix), kLivePrefix) == 0) {
        identify(f, [this](std::shared_ptr<Flow> flow) { serveChannel(std::move(flow)); });
        return;
    }
    f->decision = route(f->admitted, *f->app);
    setRenderFormat(f->render, f->decision);
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
    SubmissionError bodyError;
    std::optional<Submission> submission = readSubmission(f->request, SubmissionLimits{}, bodyError);
    if (!submission) {
        f->send(HttpResponse::text(bodyError.status, "Bad Request: " + bodyError.message + "\n"), false);
        return;
    }
    std::optional<std::string> csrfToken;
    RequestFacts facts = requestFacts(f->request, f->admitted, &*submission, &csrfToken);
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
    a.addressGroup = addressGroup(f->client);
    a.user = f->user;
    a.app = f->app;
    a.config = f->config;
    a.render = f->render;
    a.origin = origin;
    a.https = options.https;
    options.authRoutes->handle(std::move(a), [f](HttpResponse response) { f->send(std::move(response)); });
}

void Engine::serveChannel(std::shared_ptr<Flow> f) const {
    const EngineOptions& options = *options_;
    const CompiledApp& app = *f->app;
    const HttpRequest& request = f->request;
    std::string name = f->admitted.path.path.substr(std::char_traits<char>::length(kLivePrefix));
    std::vector<std::string> sources;
    std::optional<AccessLevel> access = options.push ? app.channelAccess(name, &sources) : std::nullopt;
    if (!access || f->admitted.path.trailingSlash) {
        // No rule covers it: not served, like a path no route matches.
        RouteDecision none;
        none.kind = RouteDecision::Kind::NotFound;
        f->send(dispatch(none, app, code_, f->render));
        return;
    }
    if (request.method != "GET") {
        HttpResponse r = HttpResponse::text(405, "Method Not Allowed\n");
        r.headers.set("Allow", "GET");
        f->send(std::move(r), false);
        return;
    }
    // The gate decides, as for a page: the access rules, and any rate limits.
    RouteEntry entry{RouteKind::Page, RelPath()};
    entry.access = *access;
    entry.accessSources = sources;
    f->personal = !access->isPublic() || f->user.has_value();
    GateFacts gateFacts;
    gateFacts.method = request.method;
    if (const std::string* origin = request.headers.get("Origin")) gateFacts.origin = *origin;
    const std::string* host = request.headers.get("Host");
    gateFacts.ownOrigin = std::string(options.https ? "https://" : "http://") + (host ? *host : "");
    gateFacts.csrfCookie = f->csrfCookie;
    if (f->user) gateFacts.visitor = {true, f->user->roles};
    gateFacts.addressGroup = addressGroup(f->client);
    if (f->user) gateFacts.userId = f->user->id;
    gateFacts.sessionHash = f->sessionHash;
    gateFacts.nowMs = options.clock->nowMillis();
    GateDecision gate = gate_.check(entry, gateFacts, nullptr);
    if (!gate.approval) {
        if (options.log) {
            options.log->write("access", request.method + " " + f->admitted.path.path + " from " + f->client + ": " +
                                             gate.reason);
        }
        // An EventSource can't follow a sign-in redirect: a plain refusal.
        int status = gate.denial == GateDecision::Denial::NeedLogin ? 401 : gate.status ? gate.status : 403;
        f->send(HttpResponse::text(status, gate.denial == GateDecision::Denial::NeedLogin
                                               ? "Sign in to subscribe to this channel.\n"
                                               : "Not allowed.\n"),
                false);
        return;
    }
    if (options.push->full()) {
        HttpResponse r = HttpResponse::text(503, "Too many subscribers; try again later.\n");
        r.headers.set("Retry-After", "30");
        f->send(std::move(r), false);
        return;
    }
    HttpResponse r;
    r.headers.set("Content-Type", "text/event-stream");
    r.headers.set("Cache-Control", "no-store");
    r.headers.set("X-Accel-Buffering", "no");   // nginx: don't hold the stream back
    std::shared_ptr<PushHub> hub = options.push;
    r.stream = [hub, name](std::shared_ptr<StreamWriter> writer) { hub->subscribe(name, std::move(writer)); };
    f->send(std::move(r), false);
}

Engine::JobServices Engine::jobServices() const {
    const EngineOptions& options = *options_;
    JobServices s;
    std::shared_ptr<OutboundClient> outbound;
    {
        std::lock_guard<std::mutex> lock(appMutex_);
        s.app = app_;
        s.config = config_;
        outbound = outbound_;
    }
    RunContext& c = s.context;
    c.data = options.data;
    c.dropbox = options.dropbox;
    if (options.db && s.app && s.app->usesDatabase()) {
        c.db = options.db;
        c.queries = &s.app->queries;   // the app, pinned with the run, keeps it alive
    }
    c.users = users_.get();
    c.rates = rates_.get();
    c.rateIp = c.rateUser = c.rateSession = "job";
    c.outbound = std::move(outbound);
    c.mailer = std::make_shared<EngineMail>(s.config, options.auth, options.rates, options.log);
    c.enqueue = options.jobs != nullptr;
    c.publish = options.push != nullptr || options.publishWithoutPush;
    c.budget = s.config->jobs.budget;
    if (s.app) c.render = renderOptionsFor(*s.app, "");
    return s;
}

Status Engine::commitEffects(const HandlerOutcome& outcome, const std::string& source) const {
    const EngineOptions& options = *options_;
    if (!outcome.enqueued.empty()) {
        if (!options.jobs) return makeError(ErrorCode::Denied, "there's no job queue here");
        std::vector<NewJob> jobs;
        std::vector<std::string> names;
        int64_t now = options.clock->nowMillis();
        for (const JobRequest& j : outcome.enqueued) {
            NewJob n;
            n.name = j.name;
            n.argsJson = toJson(j.args);
            n.key = j.key;
            n.runAtMs = now + j.delayMs;
            n.source = source;
            jobs.push_back(std::move(n));
            names.push_back(j.name);
        }
        auto added = options.jobs->enqueue(jobs);
        if (!added) {
            if (options.log) options.log->write("jobs", source + ": enqueuing failed: " + added.error().message);
            return makeError(added.error().code, "the enqueued jobs couldn't be saved: " + added.error().message);
        }
        if (options.observer) options.observer->jobsEnqueued(source, names, true);
        if (options.jobsAdded) options.jobsAdded();
    }
    if (options.push) {
        for (const PushMessage& m : outcome.published) options.push->publish(m.channel, m.event, m.data);
    }
    return {};
}

void Engine::dropEffects(const std::vector<std::string>& names, const std::string& source) const {
    const EngineOptions& options = *options_;
    if (names.empty()) return;
    if (options.log) {
        options.log->write("jobs", source + " failed, so its " + std::to_string(names.size()) +
                                       (names.size() == 1 ? " enqueued job was" : " enqueued jobs were") + " dropped");
    }
    if (options.observer) options.observer->jobsEnqueued(source, names, false);
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

    // Parse: the body, under limits, before the gate (the CSRF token may be
    // in it). A route with file fields may take its uploads on top.
    size_t allowed = options.maxFormBytes + (entry->form ? entry->form->maxUploadBytes() + kMultipartOverhead : 0);
    if (request.body.size() > allowed) {
        f->send(HttpResponse::text(413, "Payload Too Large\n"), false);
        return;
    }
    SubmissionLimits limits;
    limits.maxFormBytes = options.maxFormBytes;
    SubmissionError bodyError;
    auto submission = std::make_shared<std::optional<Submission>>(readSubmission(request, limits, bodyError));
    if (!*submission) {
        f->send(HttpResponse::text(bodyError.status, (bodyError.status == 413 ? "Payload Too Large: " : "Bad Request: ") +
                                                         bodyError.message + "\n"),
                false);
        return;
    }

    // Gate: nothing is served without the policy's approval.
    f->personal = !entry->access.isPublic() || f->user.has_value();
    std::optional<std::string> csrfToken;
    auto facts = std::make_shared<RequestFacts>(requestFacts(request, f->admitted, &**submission, &csrfToken));
    GateFacts gateFacts;
    gateFacts.method = request.method;
    if (const std::string* origin = request.headers.get("Origin")) gateFacts.origin = *origin;
    const std::string* host = request.headers.get("Host");
    gateFacts.ownOrigin = std::string(options.https ? "https://" : "http://") + (host ? *host : "");
    gateFacts.csrfCookie = f->csrfCookie;
    gateFacts.csrfToken = csrfToken;
    if (f->user) gateFacts.visitor = {true, f->user->roles};
    gateFacts.addressGroup = addressGroup(f->client);
    if (f->user) gateFacts.userId = f->user->id;
    gateFacts.sessionHash = f->sessionHash;
    gateFacts.defaultLimits = f->user ? f->config->limits.signedIn : f->config->limits.anonymous;
    gateFacts.formTime = (*submission)->formTime;
    gateFacts.challengeAnswer = (*submission)->challenge;
    gateFacts.challengeBits = f->config->limits.challengeBits;
    gateFacts.nowMs = options.clock->nowMillis();

    auto next = [this, f, facts, submission](GateDecision gate) {
        afterGate(f, std::move(gate), std::move(*facts), std::move(**submission));
    };
    if (options.rates && scripts_ && PolicyGate::needsStore(*entry, request.method)) {
        // Counting is SQLite: on a worker.
        const RouteEntry* e = entry;
        RateStore* rates = options.rates;
        const PolicyGate* gate = &gate_;
        scripts_->workers().submit([gate, e, gateFacts, rates, next] { next(gate->check(*e, gateFacts, rates)); });
        return;
    }
    next(gate_.check(*entry, gateFacts, options.rates));
}

void Engine::afterGate(std::shared_ptr<Flow> f, GateDecision gate, RequestFacts facts, Submission submission) const {
    const EngineOptions& options = *options_;
    const CompiledApp& app = *f->app;
    const RouteDecision& decision = f->decision;
    const RouteEntry* entry = decision.entry;
    const HttpRequest& request = f->request;
    if (options.observer && !gate.counted.empty()) options.observer->limitsCounted(request.method + " " +
                                                                                    f->admitted.path.path, gate.counted);
    if (!gate.approval) {
        using Denial = GateDecision::Denial;
        const char* category = gate.denial == Denial::Csrf                                    ? "csrf"
                               : gate.denial == Denial::NeedLogin || gate.denial == Denial::Forbidden ? "access"
                               : gate.denial == Denial::RateLimited                           ? "limits"
                                                                                              : "spam";
        if (options.log) options.log->write(category, request.method + " " + f->admitted.path.path + " from " +
                                                          f->client + ": " + gate.reason);
        bool htmx = isHtmxRequest(request);
        if (gate.denial == Denial::NeedLogin && (request.method == "GET" || request.method == "HEAD")) {
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
        if (gate.denial != Denial::NeedLogin && gate.denial != Denial::Forbidden && options.observer) {
            options.observer->scriptError({"request refused: " + gate.reason, "", 0});
        }
        if (gate.denial == Denial::RateLimited || gate.denial == Denial::TooFast) {
            HttpResponse r;
            if (app.tooManyPage && gate.denial == Denial::RateLimited) {
                if (const CompiledTemplate* t = app.templates.find(*app.tooManyPage)) {
                    RenderOptions quiet = f->render;
                    quiet.warnings = false;
                    r.status = 429;
                    r.headers.set("Content-Type", "text/html; charset=utf-8");
                    r.body = render(*t, EmptyFields(), quiet).html.str();
                }
            }
            if (r.body.empty()) {
                r = page(429, gate.denial == Denial::TooFast ? kTooFastPage : kTooManyPage, gate.reason, options);
            }
            r.headers.set("Retry-After", std::to_string(gate.retryAfterSeconds));
            f->send(std::move(r));
            return;
        }
        const char* html = gate.denial == Denial::NeedLogin   ? kSignInPage
                           : gate.denial == Denial::Forbidden ? kNotAllowedPage
                           : gate.denial == Denial::Challenge ? kNeedsJsPage
                                                              : kForbiddenPage;
        f->send(page(gate.status, html, gate.reason, options));
        return;
    }

    // A declared form: only its fields, normalized and typed, and its
    // uploads, checked by content (overview §4, Forms and validation).
    if (entry->form && request.method == "POST") {
        CheckedForm checked = checkForm(*entry->form, submission);
        facts.form.clear();
        facts.declaredForm = std::move(checked.form);
        facts.errors = std::move(checked.errors);
        facts.files = std::move(checked.files);
        facts.typed = std::move(checked.typed);
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
    facts.ip = f->client;
    if (decision.handler->kind == HandlerRef::Kind::Webhook) {
        // The body as sent, for checking the caller's signature, and parsed
        // if it's JSON.
        facts.rawBody = request.body;
        const std::string* type = request.headers.get("Content-Type");
        if (type && isJsonMediaType(*type)) {
            auto parsed = parseJson(request.body);
            facts.json = parsed ? std::move(parsed).value() : Datum();
        }
    }
    context.request = std::move(facts);
    context.request.format = decision.markdown ? "markdown" : "html";
    if (f->user) context.request.user = userFacts(*f->user);
    context.render = f->render;
    context.data = options.data;
    context.dropbox = options.dropbox;
    if (options.db && app.usesDatabase()) {
        context.db = options.db;
        context.queries = &app.queries;   // the app pinned with the run keeps it alive
    }
    context.users = users_.get();
    context.rates = rates_.get();
    context.outbound = f->outbound;
    context.mailer = std::make_shared<EngineMail>(f->config, options.auth, options.rates, options.log);
    context.enqueue = options.jobs != nullptr;
    context.publish = options.push != nullptr || options.publishWithoutPush;
    std::string group = addressGroup(f->client);
    context.rateIp = "ip:" + group;
    context.rateUser = f->user ? "user:" + f->user->id : "ip:" + group;
    context.rateSession = f->sessionHash ? "session:" + toHex(*f->sessionHash) : "ip:" + group;
    context.form = entry->form;
    context.pageUrl = entry->pageUrl;
    std::shared_ptr<EngineSession> session;
    if (useSession) {
        session = std::make_shared<EngineSession>(*options.sessions, *options.sessionLocks, scripts_->workers(),
                                                  f->sessionHash, addressGroup(f->client), options.https);
        context.session = session;
        context.sessionMaxBytes = options.sessions->config().maxBytes;
    }

    // The route entry lives in the app, which `f` keeps alive.
    bool htmx = isHtmxRequest(request);
    WorkerPool* workers = &scripts_->workers();
    std::string source = request.method + " " + f->admitted.path.path;
    scripts_->run(
        f->app, *decision.handler, *gate.approval, std::move(context),
        [this, f, entry, htmx, session, workers, source](HandlerOutcome outcome) {
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
            if (!outcome.droppedJobs.empty()) dropEffects(outcome.droppedJobs, source);
            bool effects = !outcome.enqueued.empty() || !outcome.published.empty();
            if (!session && !effects) {
                respondWith(std::move(outcome), std::nullopt);
                return;
            }
            // Commit: save what the run did to the session, then its jobs and
            // push messages (SQLite, so on a worker), before the response
            // goes out. If the session can't be saved, nothing else is kept.
            auto shared = std::make_shared<HandlerOutcome>(std::move(outcome));
            workers->submit([this, session, shared, respondWith, source] {
                std::optional<std::string> cookie;
                if (session) {
                    EngineSession::Committed c = session->commit(shared->session);
                    if (c.error) {
                        std::vector<std::string> names;
                        for (const JobRequest& j : shared->enqueued) names.push_back(j.name);
                        dropEffects(names, source);
                        HandlerOutcome failed;
                        failed.kind = HandlerOutcome::Kind::Error;
                        failed.error.message = "the session couldn't be saved: " + c.error->message;
                        respondWith(std::move(failed), std::nullopt);
                        return;
                    }
                    cookie = c.setCookie;
                }
                if (Status s = commitEffects(*shared, source); !s) {
                    HandlerOutcome failed;
                    failed.kind = HandlerOutcome::Kind::Error;
                    failed.error.message = s.error().message;
                    respondWith(std::move(failed), cookie);
                    return;
                }
                respondWith(std::move(*shared), cookie);
            });
        });
}

}  // namespace fourfold
