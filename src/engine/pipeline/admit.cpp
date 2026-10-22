#include "pipeline/stages.h"

namespace fourfold {

namespace {

// "example.com:8080" → "example.com"; "[::1]:8080" → "[::1]".
std::string hostName(const std::string& host) {
    if (!host.empty() && host[0] == '[') {
        size_t close = host.find(']');
        return close == std::string::npos ? host : host.substr(0, close + 1);
    }
    return host.substr(0, host.find(':'));
}

HttpResponse badRequest(const std::string& why) {
    return HttpResponse::text(400, "Bad Request: " + why + "\n");
}

}  // namespace

std::vector<std::string> localhostNames() {
    return {"localhost", "127.0.0.1", "[::1]"};
}

std::optional<HttpResponse> admit(const HttpRequest& request, const EngineOptions& options,
                                  AdmittedRequest& out) {
    int hostCount = 0;
    for (const Headers::Field& f : request.headers) {
        if (equalsIgnoreCase(f.name, "Host")) hostCount++;
    }
    if (hostCount > 1) return badRequest("more than one Host header");
    const std::string* host = request.headers.get("Host");
    if (!host && (request.versionMinor >= 1 || !options.allowedHosts.empty())) {
        return badRequest("no Host header");
    }
    if (host && !options.allowedHosts.empty()) {
        std::string name = hostName(*host);
        bool allowed = false;
        for (const std::string& h : options.allowedHosts) allowed = allowed || equalsIgnoreCase(name, h);
        if (!allowed) return badRequest("unknown host");
    }

    auto path = canonicalUrlPath(request.target);
    if (!path) return badRequest("invalid path");

    out.method = request.method;
    out.path = std::move(path).value();
    size_t question = request.target.find('?');
    out.hasQuery = question != std::string::npos;
    if (out.hasQuery) {
        size_t hash = request.target.find('#', question);
        out.query = request.target.substr(question + 1, hash == std::string::npos ? hash : hash - question - 1);
    } else {
        out.query.clear();
    }
    return std::nullopt;
}

}  // namespace fourfold
