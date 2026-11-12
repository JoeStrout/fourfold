// OutboundClient: the app's HTTP requests to other servers (Arch §5
// outbound; overview §6).
//
// Under the threat model app code is trusted, but what it sends may be
// steered by a visitor (a URL to preview, a host name). So every request
// goes through the same checks, which app code can't change:
//
// - the destination must be on the app's allowlist (code/outbound.ms), and
//   on the config's cap if it has one: scheme, host and port;
// - every address connected to is checked as the connection is made
//   (address_rules.h), so a name can't be checked as public and then
//   connect somewhere private (DNS rebinding), and a name with both kinds
//   of address is only ever connected to at its public ones;
// - redirects are followed here, not by libcurl: each hop is checked again,
//   at most 5, never from HTTPS to HTTP; a 303 (and a 301 or 302 after a
//   POST) becomes a GET;
// - responses are limited in size (after decompression) and time, and the
//   app's requests in rate;
// - no proxy from the environment, no .netrc, no cookies.
//
// Secrets (overview §6, Secrets): a URL or header value is made of parts,
// each a string or a secret's name. The secret's value is put in only here,
// as the request goes out, and only if the request's host is one of the
// secret's; a redirect to another host with a secret in its headers is
// refused. A secret can go only in a URL's path or query (percent-encoded),
// never its host. The script side holds only names, so no script bug can
// put a value in a page, a log or a data file.
//
// send() blocks, so it's for worker threads; it's thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "base/result.h"
#include "compiledapp/outbound_policy.h"
#include "platform/address.h"

namespace fourfold {

// A secret from private/config/secrets.ms (overview §10).
// - With `hosts`: app code gets a handle it can send to those hosts
//   ("api.example.com", or "*.example.com" for its subdomains).
// - With `verify`: app code gets a handle it can check signatures with
//   (hmac.verify), for webhooks; it's never sent anywhere.
// - With neither: engine-only (SMTP and database passwords), never visible
//   to app code at all.
struct Secret {
    std::string value;
    std::vector<std::string> hosts;
    bool verify = false;

    bool engineOnly() const { return hosts.empty() && !verify; }
    bool allowsHost(const std::string& host) const;
};

using SecretMap = std::map<std::string, Secret>;

// A secret in a request, by name.
struct SecretRef {
    std::string name;
    bool operator==(const SecretRef& o) const { return name == o.name; }
};

// A URL or header value: strings and secrets, joined as the request goes out.
using OutboundPart = std::variant<std::string, SecretRef>;
using OutboundParts = std::vector<OutboundPart>;

struct OutboundRequest {
    std::string method = "GET";          // GET, POST, PUT, PATCH, DELETE, HEAD
    OutboundParts url;
    std::vector<std::pair<std::string, OutboundParts>> headers;
    std::optional<std::string> body;     // sent as is
};

struct OutboundResponse {
    int status = 0;
    // Names lowercased; a repeated header's values joined with ", ".
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                    // decompressed
    std::string url;                     // the last hop's URL, redacted (see OutboundCall)
};

// The config's part (config.outbound): only ever tightens the app's own
// allowlist.
struct OutboundSettings {
    bool enabled = true;
    // A cap on the app's allowlist: if set, a destination must match one of
    // these as well.
    std::optional<std::vector<HostRule>> cap;
    // Internal addresses the app may reach despite the address rules (a
    // service on the same network). Only these; the allowlist still applies.
    std::vector<AddressRange> allowAddresses;
    int perMinute = 120;                                   // requests per minute, per app
    size_t maxResponseBytes = 5 * 1024 * 1024;             // after decompression
    std::chrono::milliseconds timeout{10'000};             // for the whole request, redirects included
    std::chrono::milliseconds connectTimeout{5'000};
    int maxRedirects = 5;
    // For tests: "host:port:address" entries, as for curl's --resolve, so a
    // test can say what a name resolves to.
    std::vector<std::string> resolve;
};

// One request, for the engine log and the dev tools. Never holds a
// secret's value: secrets appear as [name], and the query as "?…".
struct OutboundCall {
    std::string method;
    std::string url;               // redacted
    std::string host;
    int status = 0;                // 0 if no response
    int64_t durationMs = 0;
    size_t bytes = 0;
    int redirects = 0;
    std::string refused;           // why it wasn't sent, or failed; empty if it got a response
};

class OutboundClient {
public:
    // `onCall` hears about every request, sent or refused (it may be null);
    // it's called on the sending thread.
    OutboundClient(OutboundSettings settings, SecretMap secrets, std::function<void(const OutboundCall&)> onCall = {});
    ~OutboundClient();

    // Sends `request` under `policy` (the app's allowlist), within
    // `timeLeft` (the run's remaining wall-clock budget; the shorter of it
    // and the settings' timeout applies). An error says why it was refused
    // or failed; a response with any status is a success.
    Result<OutboundResponse> send(const OutboundPolicy& policy, const OutboundRequest& request,
                                  std::chrono::milliseconds timeLeft);

    // Checks `signature` (hex or base64) against the HMAC of `message` keyed
    // with the secret `secretName`, in constant time. `algorithm` is
    // "sha256". Fails if the secret can't be used for this (no such secret,
    // or an engine-only one).
    Result<bool> verifyHmac(const std::string& algorithm, const std::string& secretName, const std::string& message,
                            const std::string& signature) const;

    // The secrets app code may see, and how: name → "send" (has hosts),
    // "verify", or both ("send, verify"); engine-only ones as "engine".
    std::map<std::string, std::string> secretUses() const;
    const OutboundSettings& settings() const { return settings_; }

private:
    bool takeRate();

    OutboundSettings settings_;
    SecretMap secrets_;
    std::function<void(const OutboundCall&)> onCall_;
    std::mutex rateMutex_;
    std::vector<int64_t> recent_;   // send times (steady ms) within the last minute
};

// HMAC-SHA-256 (RFC 2104), as raw bytes.
std::string hmacSha256(std::string_view key, std::string_view message);

// --- URLs, for the client and its tests ---

struct ParsedUrl {
    bool https = true;
    std::string host;      // lowercased; an IPv6 address without brackets
    int port = 443;
    std::string target;    // "/path?query", at least "/"
    bool ipLiteral = false;
};

// A URL as the client will send it: http or https only, no user info, a
// host name or numeric address, an optional port; spaces, non-ASCII bytes
// and characters a URL can't hold are percent-encoded in the path and
// query; any #fragment is dropped.
Result<ParsedUrl> parseOutboundUrl(const std::string& url);

// Why a redirect from `from` to `to` is refused whatever the allowlist says
// (from HTTPS to plain HTTP), or "" if it isn't.
std::string redirectRefusal(const ParsedUrl& from, const ParsedUrl& to);

}  // namespace fourfold
