// Where the app may send requests and mail (overview §6, Destination
// allowlist): read from code/outbound.ms at compile time.
//
//   allow  = ["api.weather.gov", "*.stripe.com", "http://feeds.example.org", "api.example.com:8443"]
//   mailTo = ["owner@example.com", "*@example.com"]
//
// A host rule is a host name, "*." and a host name for its subdomains (not
// the name itself), or "*" for any public host; optionally "http://" in
// front, to allow plain HTTP as well as HTTPS; and optionally ":port". With
// no port, HTTPS may use 443 and HTTP 80.
//
// The config's tightening (config.outbound) uses the same rules, as a cap:
// a destination must be allowed by both.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fourfold {

struct HostRule {
    std::string host;              // lowercased; for "*.example.com", "example.com"; empty for "*"
    bool subdomains = false;       // "*.example.com"
    bool any = false;              // "*"
    bool http = false;             // "http://...": plain HTTP is allowed too
    std::optional<int> port;       // ":8443"
    std::string text;              // as written

    // Whether a request to `host` (lowercased) on `port` over `https` (or
    // not) is allowed by this rule.
    bool allows(const std::string& host, int port, bool https) const;
};

// A rule as written, or nullopt with `error` set.
std::optional<HostRule> parseHostRule(const std::string& text, std::string& error);

// A host name: letters, digits, '-' and '.', in labels of at most 63
// bytes, at most 253 in all; or a numeric IPv4 address. Lowercase it first.
bool isValidHostName(const std::string& host);

struct OutboundPolicy {
    // code/outbound.ms exists. Without it, the app may send no requests
    // and no mail but to its users.
    bool declared = false;
    std::vector<HostRule> hosts;             // allow
    std::vector<std::string> mailTo;         // lowercased: "a@b.c", "*@b.c", or "*"

    bool allowsHost(const std::string& host, int port, bool https) const;
    // Whether app mail may go to `address` (lowercased) without it being a
    // user's.
    bool allowsMailTo(const std::string& address) const;
    bool anyHost() const;
    bool anyMail() const;
};

// A mailTo entry as written ("owner@example.com", "*@example.com", "*"),
// lowercased, or nullopt with `error` set.
std::optional<std::string> parseMailRule(const std::string& text, std::string& error);

}  // namespace fourfold
