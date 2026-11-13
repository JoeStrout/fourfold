// The server side of TLS (overview §8, TLS): ServerTls holds the
// certificates, chosen by the name the client asks for (SNI), and the
// settings every connection shares; TlsSession is one connection's TLS,
// over memory buffers, so the caller does all the socket I/O itself.
//
// Settings: TLS 1.2 and 1.3 only, forward-secret AEAD ciphers only (Mozilla's
// "intermediate" profile), no renegotiation, no compression. ALPN offers
// "h2" (if HTTP/2 is on) and "http/1.1".
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/result.h"
#include "tls/certificates.h"

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

namespace fourfold {

class ServerTls {
public:
    static Result<std::shared_ptr<ServerTls>> create(bool http2 = true);
    ~ServerTls();

    ServerTls(const ServerTls&) = delete;
    ServerTls& operator=(const ServerTls&) = delete;

    // Adds a certificate chain and its key (PEM), for the names in the
    // certificate. It replaces any earlier one for the same first name.
    // Connections already open keep the one they started with.
    // Thread-safe.
    Status install(const std::string& chainPem, const std::string& keyPem);

    // The installed certificate that would be used for `host`, if any.
    // Thread-safe.
    std::optional<CertificateInfo> certificateFor(const std::string& host) const;
    bool empty() const;
    bool http2() const { return http2_; }

private:
    friend class TlsSession;
    struct Entry {
        CertificateInfo info;
        SSL_CTX* ctx;   // owned
    };

    ServerTls(SSL_CTX* base, bool http2) : base_(base), http2_(http2) {}
    // The context for `host` (SNI), or the first installed; adds a reference
    // the caller releases. Null if there's none.
    SSL_CTX* contextFor(const std::string& host) const;
    // The entry for `host`, exact names first (with mutex_ held).
    const Entry* find(const std::string& host) const;
    static int onServerName(SSL* ssl, int* alert, void* arg);

    SSL_CTX* base_;   // the context sessions start with: no certificate of its own
    bool http2_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

class TlsSession {
public:
    explicit TlsSession(std::shared_ptr<ServerTls> tls);
    ~TlsSession();

    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    enum class State {
        Open,     // carry on
        Closed,   // the client said it's done (close_notify)
        Failed,   // a TLS error: the handshake failed, or garbage arrived
    };

    // Takes bytes from the socket; appends any decrypted bytes to `plain`.
    // May produce output (the handshake, alerts): see takeOutput().
    State receive(const char* data, size_t size, std::string& plain);
    // Encrypts application bytes, for takeOutput().
    void send(const char* data, size_t size);
    // Sends close_notify.
    void close();
    // Appends the bytes waiting to go to the socket to `out`.
    void takeOutput(std::string& out);
    bool hasOutput() const;

    bool handshakeDone() const;
    // The protocol ALPN chose ("h2", "http/1.1"), or "" if none was.
    std::string protocol() const;
    // The name the client asked for (SNI), or "".
    std::string serverName() const;
    // Why it failed, for the log.
    const std::string& failure() const { return failure_; }

private:
    std::shared_ptr<ServerTls> tls_;
    SSL* ssl_ = nullptr;
    BIO* in_ = nullptr;    // owned by ssl_
    BIO* out_ = nullptr;   // owned by ssl_
    std::string failure_;
};

}  // namespace fourfold
