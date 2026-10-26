#include "session.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

#include "base/crypto.h"

namespace fourfold {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::optional<std::string> cookieValue(const HttpRequest& request, const std::string& name) {
    for (const auto& field : request.headers) {
        if (!equalsIgnoreCase(field.name, "Cookie")) continue;
        std::string_view rest = field.value;
        while (!rest.empty()) {
            size_t semi = rest.find(';');
            std::string_view pair = trim(rest.substr(0, semi));
            rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
            size_t eq = pair.find('=');
            if (eq == std::string_view::npos) continue;
            if (trim(pair.substr(0, eq)) != name) continue;
            std::string_view value = trim(pair.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return std::string(value);   // the first one wins, as browsers send the most specific first
        }
    }
    return std::nullopt;
}

std::string cookieName(const std::string& base, bool https) { return (https ? "__Host-ff_" : "ff_") + base; }

std::string setCookie(const std::string& base, const std::string& value, bool https, int64_t maxAgeSeconds) {
    std::string s = cookieName(base, https) + "=" + value + "; Path=/; Max-Age=" + std::to_string(maxAgeSeconds) +
                    "; HttpOnly; SameSite=Lax";
    if (https) s += "; Secure";
    return s;
}

std::string addressGroup(const std::string& peerAddress) {
    in6_addr v6;
    if (peerAddress.find(':') != std::string::npos && inet_pton(AF_INET6, peerAddress.c_str(), &v6) == 1) {
        // An IPv4-mapped address (::ffff:1.2.3.4) is really IPv4.
        static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(v6.s6_addr, kMapped, 12) == 0) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, v6.s6_addr + 12, buf, sizeof buf);
            return buf;
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64", v6.s6_addr[0], v6.s6_addr[1],
                      v6.s6_addr[2], v6.s6_addr[3], v6.s6_addr[4], v6.s6_addr[5], v6.s6_addr[6], v6.s6_addr[7]);
        return buf;
    }
    return peerAddress;
}

EngineSession::EngineSession(SessionStore& store, LockService& locks, WorkerPool& workers,
                             std::optional<std::string> idHash, std::string addressGroup, bool https)
    : store_(store), locks_(locks), workers_(workers), idHash_(std::move(idHash)),
      addressGroup_(std::move(addressGroup)), https_(https) {}

EngineSession::~EngineSession() {
    // commit() releases the lock; this is for a run that never got that far
    // (the host stopped).
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticket_) locks_.release(*ticket_);
}

void EngineSession::open(std::function<void(Result<SessionInput>)> ready) {
    if (!idHash_) {
        ready(SessionInput{});   // a new visitor: nothing to load or lock
        return;
    }
    std::string key = "session:" + toHex(*idHash_);
    auto self = shared_from_this();
    LockService::Ticket ticket = locks_.acquire(key, [self, this, ready](Status locked, LockService::Ticket) {
        if (!locked) {
            ready(locked.error());
            return;
        }
        workers_.submit([self, this, ready] {
            auto record = store_.load(*idHash_);
            if (!record) {
                ready(record.error());
                return;
            }
            SessionInput input;
            if (record.value()) {
                auto app = parseJson(record.value()->app);
                if (app && app.value().kind() == Datum::Kind::Map) input.app = std::move(app).value();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                record_ = std::move(record).value();
            }
            ready(std::move(input));
        });
    });
    std::lock_guard<std::mutex> lock(mutex_);
    ticket_ = ticket;
}

EngineSession::Committed EngineSession::commit(const std::optional<SessionResult>& result) {
    Committed out;
    std::optional<SessionRecord> record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record = record_;
    }
    if (result) {
        // The engine's part holds nothing a run can change (yet: M6 keeps
        // the signed-in user there), so it's saved as it was loaded.
        Status saved;
        if (record) {
            if (result->app != record->app) {
                saved = store_.save(record->idHash, record->engine, result->app);
            } else {
                saved = store_.touch(record->idHash, record->lastUsedAt);
            }
        } else if (result->app != "{}") {
            // Anonymous sessions begin with the first write (overview §5).
            std::string id = SessionStore::newId();
            saved = store_.create(id, addressGroup_, "{}", result->app);
            if (saved) {
                const SessionConfig& c = store_.config();
                int64_t maxAge = std::chrono::duration_cast<std::chrono::seconds>(c.anonymousLimit).count();
                out.setCookie = setCookie("session", id, https_, maxAge);
            }
        }
        if (!saved) out.error = saved.error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticket_) {
        locks_.release(*ticket_);
        ticket_.reset();
    }
    return out;
}

}  // namespace fourfold
