// AuthRoutes: the engine's own sign-in pages, under /__auth/ (overview §5,
// "Built into the engine"; Arch §5 authflow). The pipeline routes requests
// there after Admit and Identify, and, for anything but GET and HEAD, after
// the CSRF check; `authflow` implements this. App code never runs there.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "compiledapp/compiled_app.h"
#include "config/app_config.h"
#include "http/message.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "template/template.h"

namespace fourfold {

struct AuthRequest {
    std::string method;
    std::string path;                    // canonical: "/__auth/login"
    RequestFacts::Pairs query;
    RequestFacts::Pairs form;            // without the CSRF token, which has been checked
    std::optional<std::string> sessionHash;   // the visitor's session cookie's, if well formed
    std::string addressGroup;            // the visitor's address (IPv4) or /64 (IPv6)
    std::optional<AuthUser> user;        // the signed-in visitor, if any
    std::shared_ptr<const CompiledApp> app;      // for frags/auth/layout.html; may be null
    std::shared_ptr<const AppConfig> config;     // never null
    RenderOptions render;                // the response's nonce and CSRF token
    std::string origin;                  // "https://example.com": where links point, without config.domain
    bool https = false;                  // cookies get Secure and the __Host- prefix
};

class AuthRoutes {
public:
    virtual ~AuthRoutes() = default;
    // Answers one request, calling `done` exactly once, on any thread.
    virtual void handle(AuthRequest request, std::function<void(HttpResponse)> done) = 0;
};

}  // namespace fourfold
