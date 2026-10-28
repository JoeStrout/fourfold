// AuthFlow: the engine's sign-in pages (overview §5; Arch §5 authflow).
//
//   GET  /__auth/login?next=/x   the sign-in form: an email address
//   POST /__auth/login           sends a sign-in link (if the address may sign in)
//   GET  /__auth/link?token=...  the link from the email: a page with a "Sign in" button
//   POST /__auth/link            signs in, then goes to `next`
//   GET  /__auth/logout          a page with sign-out buttons
//   POST /__auth/logout          signs out (everywhere=1: every device)
//   GET  /__auth/fake            dev mode: "sign in as..." a test user
//   POST /__auth/fake            signs in as one
//
// The emailed link only shows a page; signing in takes a POST from it, so a
// mail scanner that fetches links can't use up (or use) the token. "Check
// your email" is the answer whether or not the address may sign in, so the
// form doesn't reveal who has an account.
//
// Signing in rotates the session ID, carrying over the anonymous session's
// map (a shopping cart, say); signing out deletes the session. The pages are
// plain, or wrapped in the app's frags/auth/layout.html; either way the
// engine renders the form itself, so a designer can't weaken it. It never
// runs app code.
#pragma once

#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "pipeline/auth_routes.h"
#include "platform/worker_pool.h"
#include "state/auth_store.h"
#include "state/engine_log.h"
#include "state/lock_service.h"
#include "state/session_store.h"

namespace fourfold {

// A dev mode test user.
struct FakeUser {
    std::string key;     // "admin": the email is admin@fake.invalid
    std::string name;    // "Admin (fake)"
    std::vector<std::string> roles;
};

class AuthFlow : public AuthRoutes {
public:
    // All must outlive this object. `log` may be null.
    AuthFlow(AuthStore& auth, SessionStore& sessions, LockService& sessionLocks, WorkerPool& workers, EngineLog* log);

    void handle(AuthRequest request, std::function<void(HttpResponse)> done) override;

    // The fake login's users: an admin, a member with no roles, and one for
    // each other role the app's access rules name.
    static std::vector<FakeUser> fakeUsers(const CompiledApp* app);

    // Where to go after signing in or out: `next` if it's a path on this
    // site (not //host, not /__auth/), else "/".
    static std::string safeNext(const std::string& next);

private:
    struct Context;

    void loginPage(const Context& c, const std::string& message = "", int status = 200);
    void requestLink(const Context& c);
    void confirmPage(const Context& c);
    void redeem(const Context& c);
    void logoutPage(const Context& c);
    void signOut(const Context& c);
    void fakePage(const Context& c);
    void fakeSignIn(const Context& c);
    // Signs the visitor in as `user`, then redirects to `next`.
    void signIn(const Context& c, const AuthUser& user, const std::string& next);

    AuthStore& auth_;
    SessionStore& sessions_;
    LockService& locks_;
    WorkerPool& workers_;
    EngineLog* log_;
};

}  // namespace fourfold
