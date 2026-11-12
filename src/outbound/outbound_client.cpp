#include "outbound/outbound_client.h"

#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include "base/crypto.h"
#include "base/version.h"
#include "outbound/address_rules.h"

namespace fourfold {

namespace {

using Clock = std::chrono::steady_clock;

void initCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isUnreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// Characters a URL's path or query may hold as they are (RFC 3986's
// unreserved, sub-delims, ':', '@', '/', '?', and '%' for escapes already made).
bool isUrlChar(unsigned char c) {
    return isUnreserved(c) || std::strchr("!$&'()*+,;=:@/?%", c) != nullptr;
}

const char kHex[] = "0123456789ABCDEF";

std::string percentEncode(std::string_view s, bool (*keep)(unsigned char)) {
    std::string out;
    for (unsigned char c : s) {
        if (keep(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

bool isTokenChar(unsigned char c) {
    return std::isalnum(c) || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

// Headers app code can't set: the engine (or libcurl) owns them.
bool isReservedHeader(const std::string& name) {
    static const std::set<std::string> kReserved = {"host", "content-length", "transfer-encoding", "connection",
                                                    "upgrade", "te", "trailer", "keep-alive", "expect",
                                                    "proxy-authorization", "proxy-connection"};
    return kReserved.count(name) > 0;
}

// Sent to the first host only, unless a redirect stays on it (as browsers do).
bool isCredentialHeader(const std::string& name) {
    return name == "authorization" || name == "cookie";
}

std::optional<int> parsePort(const std::string& s) {
    if (s.empty() || s.size() > 5) return std::nullopt;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + (c - '0');
    }
    if (n < 1 || n > 65535) return std::nullopt;
    return n;
}

std::string urlText(const ParsedUrl& u) {
    std::string host = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    bool defaultPort = u.port == (u.https ? 443 : 80);
    return std::string(u.https ? "https://" : "http://") + host + (defaultPort ? "" : ":" + std::to_string(u.port)) +
           u.target;
}

// The URL for logs: no query.
std::string redactedText(const ParsedUrl& u) {
    ParsedUrl r = u;
    size_t q = r.target.find('?');
    if (q != std::string::npos) r.target = r.target.substr(0, q) + "?…";
    return urlText(r);
}

// --- Assembling a request from parts and secrets ---

// Secrets in a URL are marked while it's parsed, so none can be in the host.
constexpr char kMarkStart = '\x01';
constexpr char kMarkEnd = '\x02';

struct Assembled {
    std::string url;        // with secrets, percent-encoded
    std::string redacted;   // with [name] in their place
    std::set<std::string> secrets;
};

// Joins a URL's parts; a secret may be only in the path or query.
Result<Assembled> assembleUrl(const OutboundParts& parts, const SecretMap& secrets) {
    std::string marked;
    std::vector<std::string> names;
    for (const OutboundPart& p : parts) {
        if (const std::string* s = std::get_if<std::string>(&p)) {
            for (unsigned char c : *s) {
                if (c < 0x20 || c == 0x7F) {
                    return makeError(ErrorCode::Invalid, "the URL has a control character (a line break or tab?)");
                }
            }
            marked += *s;
        } else {
            const std::string& name = std::get<SecretRef>(p).name;
            marked += kMarkStart + std::to_string(names.size()) + kMarkEnd;
            names.push_back(name);
        }
    }
    size_t scheme = marked.find("://");
    size_t authorityEnd = scheme == std::string::npos ? std::string::npos : marked.find_first_of("/?#", scheme + 3);
    size_t firstMark = marked.find(kMarkStart);
    if (firstMark != std::string::npos && (authorityEnd == std::string::npos || firstMark < authorityEnd)) {
        return makeError(ErrorCode::Denied, "a secret can go only in a URL's path or query, never its host");
    }
    Assembled out;
    for (size_t i = 0; i < marked.size(); i++) {
        if (marked[i] != kMarkStart) {
            out.url += marked[i];
            out.redacted += marked[i];
            continue;
        }
        size_t end = marked.find(kMarkEnd, i);
        const std::string& name = names[std::stoul(marked.substr(i + 1, end - i - 1))];
        auto it = secrets.find(name);
        if (it == secrets.end()) return makeError(ErrorCode::NotFound, "there's no secret named " + name);
        out.url += percentEncode(it->second.value, isUnreserved);
        out.redacted += "[" + name + "]";
        out.secrets.insert(name);
        i = end;
    }
    return out;
}

struct HeaderValue {
    std::string name;    // lowercased
    std::string sent;    // as the app wrote it
    std::string value;   // with secrets
    std::set<std::string> secrets;
};

Result<std::vector<HeaderValue>> assembleHeaders(const std::vector<std::pair<std::string, OutboundParts>>& headers,
                                                 const SecretMap& secrets) {
    std::vector<HeaderValue> out;
    for (const auto& [rawName, parts] : headers) {
        std::string name = lowered(rawName);
        if (name.empty()) return makeError(ErrorCode::Invalid, "a header has an empty name");
        for (unsigned char c : name) {
            if (!isTokenChar(c)) return makeError(ErrorCode::Invalid, "\"" + rawName + "\" isn't a header name");
        }
        if (isReservedHeader(name)) {
            return makeError(ErrorCode::Denied, "the " + rawName + " header is set by the engine, not by app code");
        }
        HeaderValue h;
        h.name = name;
        h.sent = rawName;
        for (const OutboundPart& p : parts) {
            if (const std::string* s = std::get_if<std::string>(&p)) {
                h.value += *s;
            } else {
                const std::string& secret = std::get<SecretRef>(p).name;
                auto it = secrets.find(secret);
                if (it == secrets.end()) return makeError(ErrorCode::NotFound, "there's no secret named " + secret);
                h.value += it->second.value;
                h.secrets.insert(secret);
            }
        }
        for (unsigned char c : h.value) {
            if (c == '\r' || c == '\n' || c == 0) {
                return makeError(ErrorCode::Invalid, "the " + rawName + " header's value has a line break");
            }
        }
        out.push_back(std::move(h));
    }
    return out;
}

// --- One exchange with libcurl ---

struct Exchange {
    const OutboundSettings* settings = nullptr;
    std::string refusedAddress;     // set by the socket check
    bool tooLarge = false;
    bool headersTooLarge = false;
    std::string body;
    size_t headerBytes = 0;
    std::vector<std::pair<std::string, std::string>> headers;
};

curl_socket_t openSocket(void* data, curlsocktype purpose, struct curl_sockaddr* address) {
    auto* x = static_cast<Exchange*>(data);
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    std::array<unsigned char, 16> bytes{};
    if (address->family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&address->addr);
        bytes[10] = bytes[11] = 0xFF;
        std::memcpy(bytes.data() + 12, &in->sin_addr, 4);
    } else if (address->family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address->addr);
        std::memcpy(bytes.data(), in6->sin6_addr.s6_addr, 16);
    } else {
        x->refusedAddress = "an address that isn't IPv4 or IPv6";
        return CURL_SOCKET_BAD;
    }
    std::string why = blockedAddressReason(bytes, x->settings->allowAddresses);
    if (!why.empty()) {
        // Remember the first refusal; libcurl may try the name's other addresses.
        if (x->refusedAddress.empty()) x->refusedAddress = formatAddress(bytes) + " is " + why;
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

size_t onBody(char* data, size_t size, size_t count, void* user) {
    auto* x = static_cast<Exchange*>(user);
    size_t n = size * count;
    if (x->body.size() + n > x->settings->maxResponseBytes) {
        x->tooLarge = true;
        return 0;   // stops the transfer
    }
    x->body.append(data, n);
    return n;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* x = static_cast<Exchange*>(user);
    size_t n = size * count;
    x->headerBytes += n;
    if (x->headerBytes > 64 * 1024) {
        x->headersTooLarge = true;
        return 0;
    }
    std::string line(data, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.compare(0, 5, "HTTP/") == 0) {
        x->headers.clear();   // a new response (after a 100 Continue, say)
        return n;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return n;
    std::string name = lowered(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    size_t start = value.find_first_not_of(" \t");
    value = start == std::string::npos ? "" : value.substr(start);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    for (auto& [existing, v] : x->headers) {
        if (existing == name) {
            v += ", " + value;
            return n;
        }
    }
    x->headers.emplace_back(std::move(name), std::move(value));
    return n;
}

struct Hop {
    std::string method;
    ParsedUrl url;
    std::vector<HeaderValue> headers;
    std::optional<std::string> body;
};

struct HopResult {
    int status = 0;
    std::string location;   // the absolute URL of a redirect, if any
};

struct SList {
    curl_slist* list = nullptr;
    ~SList() { curl_slist_free_all(list); }
    void add(const std::string& s) { list = curl_slist_append(list, s.c_str()); }
};

Result<HopResult> exchange(const Hop& hop, const OutboundSettings& settings, Exchange& x,
                           std::chrono::milliseconds left) {
    CURL* curl = curl_easy_init();
    if (!curl) return makeError(ErrorCode::Internal, "libcurl couldn't start");
    std::unique_ptr<CURL, void (*)(CURL*)> guard(curl, curl_easy_cleanup);
    std::string url = urlText(hop.url);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");        // never a proxy from the environment
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, openSocket);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &x);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // what libcurl can decode; sizes are checked after
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &x);
    long total = std::max<long>(1, static_cast<long>(left.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, total);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     std::min<long>(total, static_cast<long>(settings.connectTimeout.count())));
    SList resolve;
    for (const std::string& r : settings.resolve) resolve.add(r);
    if (resolve.list) curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve.list);

    SList headers;
    bool userAgent = false, contentType = false;
    for (const HeaderValue& h : hop.headers) {
        // libcurl takes "Name:" as "don't send Name", and "Name;" as an empty one.
        headers.add(h.value.empty() ? h.sent + ";" : h.sent + ": " + h.value);
        userAgent = userAgent || h.name == "user-agent";
        contentType = contentType || h.name == "content-type";
    }
    if (!userAgent) headers.add(std::string("User-Agent: Fourfold/") + versionString());
    if (hop.body && !contentType) headers.add("Content-Type: application/octet-stream");
    headers.add("Expect:");   // no 100-continue round trip
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);

    if (hop.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (hop.method != "GET" || hop.body) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, hop.method.c_str());
    }
    if (hop.body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(hop.body->size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, hop.body->data());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (!x.refusedAddress.empty()) {
            return makeError(ErrorCode::Denied, hop.url.host + " was refused: " + x.refusedAddress +
                                                    ", which app code can't reach");
        }
        if (x.tooLarge) {
            return makeError(ErrorCode::TooLarge, "the response from " + hop.url.host + " is over the limit of " +
                                                      std::to_string(settings.maxResponseBytes / 1024) + " KB");
        }
        if (x.headersTooLarge) {
            return makeError(ErrorCode::TooLarge, "the response from " + hop.url.host + " has over 64 KB of headers");
        }
        if (rc == CURLE_OPERATION_TIMEDOUT) {
            return makeError(ErrorCode::IO, "the request to " + hop.url.host + " took too long (" +
                                                std::to_string(total) + " ms)");
        }
        return makeError(ErrorCode::IO, "the request to " + hop.url.host + " failed: " + curl_easy_strerror(rc));
    }
    HopResult r;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r.status = static_cast<int>(status);
    char* location = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location) == CURLE_OK && location) r.location = location;
    return r;
}

std::optional<std::string> decodeHex(const std::string& s) {
    if (s.size() % 2) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = digit(s[i]), lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>(hi * 16 + lo);
    }
    return out;
}

// Base64, standard or URL-safe, with or without padding.
std::optional<std::string> decodeBase64(std::string s) {
    while (!s.empty() && s.back() == '=') s.pop_back();
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

}  // namespace

bool Secret::allowsHost(const std::string& host) const {
    for (const std::string& h : hosts) {
        if (h == host) return true;
        if (h.compare(0, 2, "*.") == 0) {
            std::string base = h.substr(2);
            if (host.size() > base.size() + 1 && host.compare(host.size() - base.size(), base.size(), base) == 0 &&
                host[host.size() - base.size() - 1] == '.') {
                return true;
            }
        }
    }
    return false;
}

Result<ParsedUrl> parseOutboundUrl(const std::string& text) {
    ParsedUrl u;
    std::string s = text;
    size_t scheme = s.find("://");
    std::string name = scheme == std::string::npos ? "" : lowered(s.substr(0, scheme));
    if (name == "https") {
        u.https = true;
    } else if (name == "http") {
        u.https = false;
    } else {
        return makeError(ErrorCode::Invalid, "the URL must start with https:// (or http://)");
    }
    s = s.substr(scheme + 3);
    size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    size_t end = s.find_first_of("/?");
    std::string authority = s.substr(0, end);
    std::string target = end == std::string::npos ? "/" : s.substr(end);
    if (target[0] == '?') target = "/" + target;
    if (authority.find('@') != std::string::npos) {
        return makeError(ErrorCode::Invalid, "the URL has a user name or password in it; send credentials in a "
                                             "header, from a secret");
    }
    if (authority.find('\\') != std::string::npos) return makeError(ErrorCode::Invalid, "the URL's host has a backslash");
    std::string host, port;
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) return makeError(ErrorCode::Invalid, "the URL's IPv6 address isn't closed");
        host = authority.substr(1, close - 1);
        std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return makeError(ErrorCode::Invalid, "the URL's host is malformed");
            port = rest.substr(1);
        }
        if (!parseAddress(host) || host.find(':') == std::string::npos) {
            return makeError(ErrorCode::Invalid, "the URL's IPv6 address is malformed");
        }
        u.ipLiteral = true;
    } else {
        size_t colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    host = lowered(host);
    if (!host.empty() && host.back() == '.') host.pop_back();   // "example.com." is example.com
    if (port.empty()) {
        u.port = u.https ? 443 : 80;
    } else {
        auto p = parsePort(port);
        if (!p) return makeError(ErrorCode::Invalid, "the URL's port isn't a number from 1 to 65535");
        u.port = *p;
    }
    if (!u.ipLiteral) {
        // Only dotted-decimal IPv4 counts as an address; "0x7f.1" and
        // "2130706433" aren't host names either, so they're refused.
        if (!isValidHostName(host)) {
            return makeError(ErrorCode::Invalid, "\"" + host + "\" isn't a host name (international names are "
                                                 "written in their xn-- form)");
        }
        bool numeric = !host.empty();
        for (char c : host) numeric = numeric && (std::isdigit(static_cast<unsigned char>(c)) || c == '.');
        if (numeric) {
            if (!parseAddress(host)) return makeError(ErrorCode::Invalid, "\"" + host + "\" isn't an IPv4 address");
            u.ipLiteral = true;
        } else {
            size_t lastDot = host.rfind('.');
            std::string tld = lastDot == std::string::npos ? host : host.substr(lastDot + 1);
            bool digitsOnly = true;
            for (char c : tld) digitsOnly = digitsOnly && std::isdigit(static_cast<unsigned char>(c));
            if (digitsOnly) return makeError(ErrorCode::Invalid, "\"" + host + "\" isn't a host name");
        }
    }
    u.host = host;
    u.target = percentEncode(target, isUrlChar);
    return u;
}

std::string redirectRefusal(const ParsedUrl& from, const ParsedUrl& to) {
    if (from.https && !to.https) return "a redirect from " + from.host + " goes from HTTPS to plain HTTP";
    return "";
}

std::string hmacSha256(std::string_view key, std::string_view message) {
    std::string k(key);
    if (k.size() > 64) k = sha256(k);
    k.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (size_t i = 0; i < 64; i++) {
        inner[i] = static_cast<char>(k[i] ^ 0x36);
        outer[i] = static_cast<char>(k[i] ^ 0x5C);
    }
    return sha256(outer + sha256(inner + std::string(message)));
}

OutboundClient::OutboundClient(OutboundSettings settings, SecretMap secrets,
                               std::function<void(const OutboundCall&)> onCall)
    : settings_(std::move(settings)), secrets_(std::move(secrets)), onCall_(std::move(onCall)) {
    initCurl();
}

OutboundClient::~OutboundClient() = default;

bool OutboundClient::takeRate() {
    std::lock_guard<std::mutex> lock(rateMutex_);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](int64_t t) { return t <= now - 60'000; }),
                  recent_.end());
    if (static_cast<int>(recent_.size()) >= settings_.perMinute) return false;
    recent_.push_back(now);
    return true;
}

Result<OutboundResponse> OutboundClient::send(const OutboundPolicy& policy, const OutboundRequest& request,
                                              std::chrono::milliseconds timeLeft) {
    auto started = Clock::now();
    OutboundCall call;
    call.method = request.method;
    auto finish = [&](Result<OutboundResponse> r) {
        call.durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (!r) call.refused = r.error().message;
        if (onCall_) onCall_(call);
        return r;
    };
    auto refuse = [&](ErrorCode code, std::string why) { return finish(makeError(code, std::move(why))); };

    static const std::set<std::string> kMethods = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    if (!kMethods.count(request.method)) {
        return refuse(ErrorCode::Invalid, "\"" + request.method + "\" isn't a method http can send");
    }
    auto assembled = assembleUrl(request.url, secrets_);
    if (!assembled) return refuse(assembled.error().code, assembled.error().message);
    auto parsed = parseOutboundUrl(assembled.value().url);
    auto redactedParsed = parseOutboundUrl(assembled.value().redacted);
    call.url = redactedParsed ? redactedText(redactedParsed.value()) : "(a malformed URL)";
    if (!parsed) return refuse(parsed.error().code, parsed.error().message);
    call.host = parsed.value().host;
    auto headers = assembleHeaders(request.headers, secrets_);
    if (!headers) return refuse(headers.error().code, headers.error().message);

    if (!settings_.enabled) return refuse(ErrorCode::Denied, "outbound requests are turned off in the config");
    if (!policy.declared) {
        return refuse(ErrorCode::Denied, "the app has no code/outbound.ms, so it can't send requests (list the "
                                         "hosts it may reach there: allow = [\"" + call.host + "\"])");
    }

    Hop hop{request.method, parsed.value(), std::move(headers).value(), request.body};
    std::set<std::string> urlSecrets = assembled.value().secrets;
    auto deadline = started + std::min(timeLeft, settings_.timeout);
    for (int redirects = 0;; redirects++) {
        const ParsedUrl& u = hop.url;
        auto allowed = [&](const ParsedUrl& to) {
            if (!policy.allowsHost(to.host, to.port, to.https)) return false;
            if (!settings_.cap) return true;
            for (const HostRule& r : *settings_.cap) {
                if (r.allows(to.host, to.port, to.https)) return true;
            }
            return false;
        };
        std::string where = (u.https ? "https://" : "http://") + u.host +
                            (u.port == (u.https ? 443 : 80) ? "" : ":" + std::to_string(u.port));
        if (!allowed(u)) {
            std::string why = where + " isn't on the app's list of hosts";
            if (policy.allowsHost(u.host, u.port, u.https)) why = where + " isn't allowed by the config's outbound.allow";
            else if (!u.https && policy.allowsHost(u.host, u.port == 80 ? 443 : u.port, true)) {
                why = where + " is plain HTTP; code/outbound.ms allows only HTTPS to " + u.host +
                      " (write \"http://" + u.host + "\" to allow both)";
            }
            return refuse(ErrorCode::Denied, (redirects ? "a redirect to " : "") + why);
        }
        // Each secret goes only to its own hosts.
        std::set<std::string> used = redirects == 0 ? urlSecrets : std::set<std::string>();
        for (const HeaderValue& h : hop.headers) used.insert(h.secrets.begin(), h.secrets.end());
        for (const std::string& name : used) {
            const Secret& s = secrets_.at(name);
            if (!s.allowsHost(u.host)) {
                std::string why = s.engineOnly() ? "the secret " + name + " is for the engine only (it has no hosts)"
                                  : s.hosts.empty()
                                      ? "the secret " + name + " is for checking signatures only (it has no hosts)"
                                      : "the secret " + name + " may be sent only to its own hosts, not " + u.host;
                return refuse(ErrorCode::Denied, (redirects ? "a redirect to " + u.host + " was refused: " : "") + why);
            }
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) return refuse(ErrorCode::IO, "no time left for the request to " + u.host);
        if (!takeRate()) {
            return refuse(ErrorCode::TooLarge, "over the limit of " + std::to_string(settings_.perMinute) +
                                                   " outbound requests a minute");
        }

        Exchange x;
        x.settings = &settings_;
        auto result = exchange(hop, settings_, x, left);
        call.redirects = redirects;
        if (!result) return refuse(result.error().code, result.error().message);
        call.url = redirects == 0 && redactedParsed ? call.url : redactedText(u);
        call.host = u.host;
        call.status = result.value().status;
        call.bytes = x.body.size();

        int status = result.value().status;
        bool redirect = (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
                        !result.value().location.empty();
        if (!redirect) {
            OutboundResponse response;
            response.status = status;
            response.headers = std::move(x.headers);
            response.body = std::move(x.body);
            response.url = call.url;
            return finish(std::move(response));
        }
        if (redirects >= settings_.maxRedirects) {
            return refuse(ErrorCode::Denied, "more than " + std::to_string(settings_.maxRedirects) + " redirects");
        }
        auto next = parseOutboundUrl(result.value().location);
        if (!next) return refuse(ErrorCode::Denied, "a redirect from " + u.host + " to a bad URL: " + next.error().message);
        if (std::string why = redirectRefusal(u, next.value()); !why.empty()) return refuse(ErrorCode::Denied, why);
        if (status == 303 || ((status == 301 || status == 302) && hop.method == "POST")) {
            if (hop.method != "HEAD") hop.method = "GET";
            hop.body.reset();
            hop.headers.erase(std::remove_if(hop.headers.begin(), hop.headers.end(),
                                             [](const HeaderValue& h) { return h.name == "content-type"; }),
                              hop.headers.end());
        }
        if (next.value().host != u.host) {
            // Credentials without secrets stay with the first host, as in a
            // browser; secrets are checked against the new host above.
            hop.headers.erase(std::remove_if(hop.headers.begin(), hop.headers.end(),
                                             [](const HeaderValue& h) {
                                                 return h.secrets.empty() && isCredentialHeader(h.name);
                                             }),
                              hop.headers.end());
        }
        hop.url = next.value();
    }
}

Result<bool> OutboundClient::verifyHmac(const std::string& algorithm, const std::string& secretName,
                                        const std::string& message, const std::string& signature) const {
    if (lowered(algorithm) != "sha256") {
        return makeError(ErrorCode::Invalid, "\"" + algorithm + "\" isn't an algorithm hmac.verify knows (\"sha256\")");
    }
    auto it = secrets_.find(secretName);
    if (it == secrets_.end()) return makeError(ErrorCode::NotFound, "there's no secret named " + secretName);
    if (it->second.engineOnly()) {
        return makeError(ErrorCode::Denied, "the secret " + secretName + " is for the engine only; give it "
                                            "\"verify\": true in secrets.ms to check signatures with it");
    }
    std::string sig = signature;
    size_t start = sig.find_first_not_of(" \t");
    sig = start == std::string::npos ? "" : sig.substr(start);
    while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\t')) sig.pop_back();
    if (lowered(sig.substr(0, 7)) == "sha256=") sig = sig.substr(7);   // GitHub's form
    std::optional<std::string> bytes = sig.size() == 64 ? decodeHex(sig) : std::nullopt;
    if (!bytes) bytes = decodeBase64(sig);
    std::string expected = hmacSha256(it->second.value, message);
    if (!bytes || bytes->size() != expected.size()) return false;
    return constantTimeEqual(*bytes, expected);
}

std::map<std::string, std::string> OutboundClient::secretUses() const {
    std::map<std::string, std::string> out;
    for (const auto& [name, s] : secrets_) {
        if (s.engineOnly()) {
            out[name] = "engine";
        } else if (!s.hosts.empty() && s.verify) {
            out[name] = "send, verify";
        } else {
            out[name] = s.verify ? "verify" : "send";
        }
    }
    return out;
}

}  // namespace fourfold
