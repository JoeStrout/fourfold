#include "tls/server_tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cstring>

#include "openssl_util.h"

namespace fourfold {

namespace {

// Mozilla's "intermediate" TLS 1.2 ciphers: ECDHE, AEAD. (TLS 1.3's
// defaults are all fine.)
const char* const kTls12Ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

// ALPN: h2 if the client offers it (and it's on), else http/1.1. A client
// offering neither gets no ALPN answer, and speaks HTTP/1.1 anyway.
int selectProtocol(SSL*, const unsigned char** out, unsigned char* outLength, const unsigned char* in,
                   unsigned int inLength, void* arg) {
    bool http2 = arg != nullptr;
    static const unsigned char kH2[] = {2, 'h', '2'};
    static const unsigned char kHttp11[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    for (const unsigned char* want : {http2 ? kH2 : nullptr, kHttp11}) {
        if (!want) continue;
        for (unsigned int i = 0; i < inLength;) {
            unsigned int length = in[i];
            if (i + 1 + length > inLength) break;
            if (length == want[0] && std::memcmp(in + i + 1, want + 1, length) == 0) {
                *out = in + i + 1;
                *outLength = static_cast<unsigned char>(length);
                return SSL_TLSEXT_ERR_OK;
            }
            i += 1 + length;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

// The settings every context shares (the base and each certificate's).
SSL_CTX* newContext(bool http2) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    // No session resumption: simpler, and with keep-alive and HTTP/2 a
    // visitor's handshakes are few.
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
    if (SSL_CTX_set_cipher_list(ctx, kTls12Ciphers) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }
    SSL_CTX_set_alpn_select_cb(ctx, selectProtocol, http2 ? ctx : nullptr);
    return ctx;
}

}  // namespace

Result<std::shared_ptr<ServerTls>> ServerTls::create(bool http2) {
    SSL_CTX* base = newContext(http2);
    if (!base) return openSslFailure(ErrorCode::Internal, "can't set up TLS");
    std::shared_ptr<ServerTls> tls(new ServerTls(base, http2));
    SSL_CTX_set_tlsext_servername_callback(base, onServerName);
    SSL_CTX_set_tlsext_servername_arg(base, tls.get());
    return tls;
}

ServerTls::~ServerTls() {
    for (Entry& e : entries_) SSL_CTX_free(e.ctx);
    SSL_CTX_free(base_);
}

Status ServerTls::install(const std::string& chainPem, const std::string& keyPem) {
    auto info = inspectCertificate(chainPem);
    if (!info) return info.error();
    if (info.value().names.empty()) return makeError(ErrorCode::Invalid, "the certificate names no host");
    if (Status s = checkKeyMatches(chainPem, keyPem); !s) return s;

    SSL_CTX* ctx = newContext(http2_);
    if (!ctx) return openSslFailure(ErrorCode::Internal, "can't set up TLS");
    UniqueBio bio(BIO_new_mem_buf(chainPem.data(), static_cast<int>(chainPem.size())));
    UniqueX509 leaf(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
    bool ok = leaf && SSL_CTX_use_certificate(ctx, leaf.get()) == 1;
    // The rest of the chain: intermediates.
    while (ok) {
        X509* extra = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
        if (!extra) {
            ERR_clear_error();   // the end of the PEM
            break;
        }
        if (SSL_CTX_add0_chain_cert(ctx, extra) != 1) {
            X509_free(extra);
            ok = false;
        }
    }
    UniqueKey key = readPrivateKey(keyPem);
    ok = ok && key && SSL_CTX_use_PrivateKey(ctx, key.get()) == 1 && SSL_CTX_check_private_key(ctx) == 1;
    if (!ok) {
        SSL_CTX_free(ctx);
        return openSslFailure(ErrorCode::Invalid, "can't use the certificate");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string& first = info.value().names[0];
    for (Entry& e : entries_) {
        if (e.info.names[0] == first) {
            SSL_CTX_free(e.ctx);   // sessions using it hold their own reference
            e = Entry{std::move(info).value(), ctx};
            return {};
        }
    }
    entries_.push_back(Entry{std::move(info).value(), ctx});
    return {};
}

std::optional<CertificateInfo> ServerTls::certificateFor(const std::string& host) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = find(host);
    if (!e) return std::nullopt;
    return e->info;
}

bool ServerTls::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

const ServerTls::Entry* ServerTls::find(const std::string& host) const {
    // An exact name before a wildcard.
    for (bool wildcards : {false, true}) {
        for (const Entry& e : entries_) {
            for (const std::string& n : e.info.names) {
                if ((n.compare(0, 2, "*.") == 0) == wildcards && hostMatches(n, host)) return &e;
            }
        }
    }
    return nullptr;
}

SSL_CTX* ServerTls::contextFor(const std::string& host) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return nullptr;
    const Entry* chosen = host.empty() ? nullptr : find(host);
    if (!chosen) chosen = &entries_[0];
    SSL_CTX_up_ref(chosen->ctx);
    return chosen->ctx;
}

int ServerTls::onServerName(SSL* ssl, int* alert, void* arg) {
    auto* tls = static_cast<ServerTls*>(arg);
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    SSL_CTX* ctx = tls->contextFor(name ? name : "");
    if (!ctx) {
        *alert = SSL_AD_UNRECOGNIZED_NAME;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    SSL_set_SSL_CTX(ssl, ctx);   // takes its own reference
    SSL_CTX_free(ctx);
    return SSL_TLSEXT_ERR_OK;
}

TlsSession::TlsSession(std::shared_ptr<ServerTls> tls) : tls_(std::move(tls)) {
    ssl_ = SSL_new(tls_->base_);
    in_ = BIO_new(BIO_s_mem());
    out_ = BIO_new(BIO_s_mem());
    if (!ssl_ || !in_ || !out_) {
        failure_ = openSslError("can't start TLS");
        if (in_) BIO_free(in_);
        if (out_) BIO_free(out_);
        in_ = out_ = nullptr;
        return;
    }
    BIO_set_mem_eof_return(in_, -1);   // empty means "wait", not EOF
    SSL_set_bio(ssl_, in_, out_);      // ssl_ owns them now
    SSL_set_accept_state(ssl_);
}

TlsSession::~TlsSession() {
    if (ssl_) SSL_free(ssl_);
}

TlsSession::State TlsSession::receive(const char* data, size_t size, std::string& plain) {
    if (!in_) return State::Failed;
    if (size > 0 && BIO_write(in_, data, static_cast<int>(size)) != static_cast<int>(size)) {
        failure_ = openSslError("TLS buffer");
        return State::Failed;
    }
    char buffer[16 * 1024];
    for (;;) {
        int n = SSL_read(ssl_, buffer, sizeof buffer);
        if (n > 0) {
            plain.append(buffer, static_cast<size_t>(n));
            continue;
        }
        int error = SSL_get_error(ssl_, n);
        switch (error) {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:   // memory BIOs never fill; output is in out_
                return State::Open;
            case SSL_ERROR_ZERO_RETURN:
                return State::Closed;
            default:
                failure_ = openSslError(handshakeDone() ? "TLS error" : "TLS handshake failed");
                return State::Failed;
        }
    }
}

void TlsSession::send(const char* data, size_t size) {
    if (!ssl_ || size == 0) return;
    // Memory BIOs take everything; a write fails only once the session has.
    while (size > 0) {
        int chunk = static_cast<int>(std::min<size_t>(size, 1 << 30));
        int n = SSL_write(ssl_, data, chunk);
        if (n <= 0) {
            ERR_clear_error();
            return;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
}

void TlsSession::close() {
    if (!ssl_ || !handshakeDone()) return;
    SSL_shutdown(ssl_);
    ERR_clear_error();
}

void TlsSession::takeOutput(std::string& out) {
    if (!out_) return;
    char* data = nullptr;
    long size = BIO_get_mem_data(out_, &data);
    if (size <= 0) return;
    out.append(data, static_cast<size_t>(size));
    (void)BIO_reset(out_);
}

bool TlsSession::hasOutput() const {
    return out_ && BIO_ctrl_pending(out_) > 0;
}

bool TlsSession::handshakeDone() const {
    return ssl_ && SSL_is_init_finished(ssl_);
}

std::string TlsSession::protocol() const {
    const unsigned char* data = nullptr;
    unsigned int length = 0;
    if (ssl_) SSL_get0_alpn_selected(ssl_, &data, &length);
    return data ? std::string(reinterpret_cast<const char*>(data), length) : std::string();
}

std::string TlsSession::serverName() const {
    const char* name = ssl_ ? SSL_get_servername(ssl_, TLSEXT_NAMETYPE_host_name) : nullptr;
    return name ? name : "";
}

}  // namespace fourfold
