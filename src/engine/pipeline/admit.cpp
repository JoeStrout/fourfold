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

std::string clientAddress(const HttpRequest& request, const std::vector<AddressRange>& trustedProxies) {
    auto trusted = [&](const std::string& address) {
        for (const AddressRange& r : trustedProxies) {
            if (r.contains(address)) return true;
        }
        return false;
    };
    std::string address = request.peerAddress;
    if (trustedProxies.empty() || !trusted(address)) return address;
    // Every X-Forwarded-For field, in order, as one list.
    std::vector<std::string> hops;
    for (const Headers::Field& f : request.headers) {
        if (!equalsIgnoreCase(f.name, "X-Forwarded-For")) continue;
        size_t start = 0;
        while (start <= f.value.size()) {
            size_t comma = f.value.find(',', start);
            if (comma == std::string::npos) comma = f.value.size();
            std::string hop = f.value.substr(start, comma - start);
            size_t a = hop.find_first_not_of(" \t"), b = hop.find_last_not_of(" \t");
            hops.push_back(a == std::string::npos ? "" : hop.substr(a, b - a + 1));
            start = comma + 1;
        }
    }
    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
        if (!parseAddress(*it)) return address;   // garbage: stop at the last address we could trust
        address = *it;
        if (!trusted(address)) return address;
    }
    return address;
}

}  // namespace fourfold
