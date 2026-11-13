#include "pipeline/stages.h"

#include <algorithm>

#include "platform/socket.h"

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

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "a, b,c" → {"a", "b", "c"}, trimmed. (Forwarded's quoted strings never
// hold a comma that matters: an address has none.)
std::vector<std::string> splitList(const std::string& value, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t at = value.find(separator, start);
        if (at == std::string::npos) at = value.size();
        out.push_back(trimmed(value.substr(start, at - start)));
        start = at + 1;
    }
    return out;
}

// The address in one Forwarded element's for= ("for=192.0.2.60;proto=https",
// "for=\"[2001:db8::1]:4711\""), or "" (none, "unknown", an obfuscated
// name), which stops the walk.
std::string forwardedFor(const std::string& element) {
    for (const std::string& pair : splitList(element, ';')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos || !equalsIgnoreCase(trimmed(pair.substr(0, eq)), "for")) continue;
        std::string v = trimmed(pair.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (!v.empty() && v[0] == '[') {
            size_t close = v.find(']');
            return close == std::string::npos ? "" : v.substr(1, close - 1);
        }
        // An IPv4 address may carry a port: "192.0.2.43:47011".
        if (std::count(v.begin(), v.end(), ':') == 1) v = v.substr(0, v.find(':'));
        return v;
    }
    return "";
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
        // A Unix-domain socket's peer: whoever its file's permissions let
        // connect, which is fourfold front (or the admin's own proxy).
        if (address == kUnixPeer) return true;
        for (const AddressRange& r : trustedProxies) {
            if (r.contains(address)) return true;
        }
        return false;
    };
    std::string address = request.peerAddress;
    if (!trusted(address)) return address;
    // The hops, nearest last: from Forwarded (RFC 7239) if the proxy sent
    // it, else X-Forwarded-For; every field of the header, in order.
    bool standard = request.headers.has("Forwarded");
    std::vector<std::string> hops;
    for (const Headers::Field& f : request.headers) {
        if (!equalsIgnoreCase(f.name, standard ? "Forwarded" : "X-Forwarded-For")) continue;
        for (const std::string& element : splitList(f.value, ',')) {
            hops.push_back(standard ? forwardedFor(element) : element);
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
