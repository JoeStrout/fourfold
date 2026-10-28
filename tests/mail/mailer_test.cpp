#include "mail/mailer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <future>
#include <string>
#include <thread>

#include "base/fd.h"
#include "doctest.h"

using namespace fourfold;

namespace {

// A listening socket on 127.0.0.1, on a free port.
struct Listener {
    Fd fd;
    int port = 0;

    Listener() {
        fd = Fd(socket(AF_INET, SOCK_STREAM, 0));
        REQUIRE(fd.valid());
        int one = 1;
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        REQUIRE(listen(fd.get(), 4) == 0);
        socklen_t len = sizeof addr;
        getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }
};

void sendAll(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// Reads up to and including `end`; "" if the connection closes first.
std::string readUntil(int fd, std::string& buffer, const std::string& end) {
    for (;;) {
        size_t at = buffer.find(end);
        if (at != std::string::npos) {
            std::string out = buffer.substr(0, at + end.size());
            buffer.erase(0, at + end.size());
            return out;
        }
        char chunk[4096];
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) return "";
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

// Accepts one connection and speaks just enough SMTP to take one message.
// Returns the commands and data it got.
std::string fakeSmtp(Listener& l, bool refuseRecipient = false) {
    Fd c(accept(l.fd.get(), nullptr, nullptr));
    if (!c.valid()) return "";
    std::string buffer, log;
    sendAll(c.get(), "220 fake ESMTP\r\n");
    for (;;) {
        std::string line = readUntil(c.get(), buffer, "\r\n");
        if (line.empty()) break;
        log += line;
        std::string verb = line.substr(0, 4);
        if (verb == "EHLO") {
            sendAll(c.get(), "250-fake\r\n250 8BITMIME\r\n");
        } else if (verb == "RCPT" && refuseRecipient) {
            sendAll(c.get(), "550 no such user\r\n");
        } else if (verb == "DATA") {
            sendAll(c.get(), "354 go ahead\r\n");
            log += readUntil(c.get(), buffer, "\r\n.\r\n");
            sendAll(c.get(), "250 queued\r\n");
        } else if (verb == "QUIT") {
            sendAll(c.get(), "221 bye\r\n");
            break;
        } else {
            sendAll(c.get(), "250 ok\r\n");
        }
    }
    return log;
}

// Accepts one HTTP request, answers with `status` and `body`, and returns
// the request.
std::string fakeHttp(Listener& l, int status, const std::string& body) {
    Fd c(accept(l.fd.get(), nullptr, nullptr));
    if (!c.valid()) return "";
    std::string buffer;
    std::string head = readUntil(c.get(), buffer, "\r\n\r\n");
    size_t at = head.find("Content-Length: ");
    size_t length = at == std::string::npos ? 0 : std::stoul(head.substr(at + 16));
    while (buffer.size() < length) {
        char chunk[4096];
        ssize_t n = ::recv(c.get(), chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
    }
    sendAll(c.get(), "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    return head + buffer;
}

MailMessage sample() {
    MailMessage m;
    m.from = "Clubhouse <noreply@example.com>";
    m.to = "ann@example.com";
    m.subject = "Your sign-in link";
    m.text = "Hello,\n.\nhttps://example.com/__auth/link?token=abc\n";
    return m;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("mail: addresses") {
    for (const char* ok : {"a@b.co", "first.last+tag@sub.example.com", "x_y-z@fake.invalid"}) {
        CAPTURE(ok);
        CHECK(isValidEmailAddress(ok));
    }
    for (const char* bad : {"", "a", "@b.co", "a@", "a@b", "a b@c.d", "a@b.co\r\nBcc: x@y.z", "<a@b.co>", "a\"@b.co",
                            ".a@b.co", "a..b@c.d", "a@b..co", "a@-b_c.d"}) {
        CAPTURE(bad);
        CHECK_FALSE(isValidEmailAddress(bad));
    }
}

TEST_CASE("mail: no header injection") {
    MailMessage m = sample();
    CHECK(validateMessage(m));
    m.subject = "Hi\r\nBcc: victim@example.com";
    CHECK_FALSE(validateMessage(m));
    m = sample();
    m.to = "ann@example.com\nBcc: x@y.z";
    CHECK_FALSE(validateMessage(m));
    m = sample();
    m.from = "Evil\r\nBcc: x@y.z <a@b.co>";
    CHECK_FALSE(validateMessage(m));
}

TEST_CASE("mail: the formatted message") {
    MailMessage m = sample();
    m.subject = "Café";
    m.text = std::string("line one\n.leading dot\ntrailing space \n") + std::string(100, 'x') + "\nnaïve =";
    std::string s = formatMessage(m, "Tue, 27 Oct 2026 14:05:00 +0000", "id@example.com");
    CHECK(contains(s, "From: Clubhouse <noreply@example.com>\r\n"));
    CHECK(contains(s, "To: <ann@example.com>\r\n"));
    CHECK(contains(s, "Subject: =?UTF-8?B?Q2Fmw6k=?=\r\n"));
    CHECK(contains(s, "Content-Transfer-Encoding: quoted-printable\r\n"));
    CHECK(contains(s, "\r\n=2Eleading dot\r\n"));
    CHECK(contains(s, "trailing space=20\r\n"));
    CHECK(contains(s, "na=C3=AFve =3D\r\n"));
    for (size_t start = 0; start < s.size();) {
        size_t end = s.find("\r\n", start);
        REQUIRE(end != std::string::npos);
        CHECK(end - start <= 76);
        CHECK(s.compare(start, 1, ".") != 0);
        start = end + 2;
    }
    CHECK(s.find('\n') == s.find("\r\n") + 1);

    m.html = "<p>Hi</p>";
    s = formatMessage(m, "Tue, 27 Oct 2026 14:05:00 +0000", "id@example.com");
    CHECK(contains(s, "multipart/alternative"));
    CHECK(contains(s, "Content-Type: text/html; charset=utf-8"));
}

TEST_CASE("mail: SMTP, to a server on this machine") {
    Listener l;
    auto server = std::async(std::launch::async, [&] { return fakeSmtp(l); });
    SmtpSettings settings;
    settings.host = "127.0.0.1";
    settings.port = l.port;
    settings.security = SmtpSettings::Security::None;
    settings.timeout = std::chrono::seconds(5);
    Status sent = makeSmtpMailer(settings)->send(sample());
    std::string log = server.get();
    REQUIRE_MESSAGE(sent, sent.error().message);
    CHECK(contains(log, "MAIL FROM:<noreply@example.com>"));
    CHECK(contains(log, "RCPT TO:<ann@example.com>"));
    CHECK(contains(log, "Subject: Your sign-in link\r\n"));
    CHECK(contains(log, "https://example.com/__auth/link?token=3Dabc"));   // quoted-printable
    CHECK(contains(log, "\r\n=2E\r\n"));   // a lone "." line can't end the message early
}

TEST_CASE("mail: SMTP failures are reported") {
    Listener l;
    auto server = std::async(std::launch::async, [&] { return fakeSmtp(l, true); });
    SmtpSettings settings;
    settings.host = "127.0.0.1";
    settings.port = l.port;
    settings.security = SmtpSettings::Security::None;
    settings.timeout = std::chrono::seconds(5);
    Status sent = makeSmtpMailer(settings)->send(sample());
    server.get();
    REQUIRE_FALSE(sent);
    CHECK(contains(sent.error().message, "127.0.0.1"));

    // STARTTLS is required: a server that doesn't offer it is refused.
    Listener l2;
    auto plain = std::async(std::launch::async, [&] { return fakeSmtp(l2); });
    settings.port = l2.port;
    settings.security = SmtpSettings::Security::StartTls;
    sent = makeSmtpMailer(settings)->send(sample());
    std::string log = plain.get();
    REQUIRE_FALSE(sent);
    CHECK_FALSE(contains(log, "DATA"));
}

TEST_CASE("mail: Postmark's API") {
    Listener l;
    auto server = std::async(std::launch::async,
                             [&] { return fakeHttp(l, 200, "{\"ErrorCode\":0,\"Message\":\"OK\"}"); });
    PostmarkSettings settings;
    settings.serverToken = "token-123";
    settings.apiUrl = "http://127.0.0.1:" + std::to_string(l.port) + "/email";
    Status sent = makePostmarkMailer(settings)->send(sample());
    std::string request = server.get();
    REQUIRE_MESSAGE(sent, sent.error().message);
    CHECK(contains(request, "POST /email HTTP/1.1"));
    CHECK(contains(request, "X-Postmark-Server-Token: token-123"));
    CHECK(contains(request, "\"To\":\"ann@example.com\""));
    CHECK(contains(request, "\"MessageStream\":\"outbound\""));

    Listener l2;
    auto refusing = std::async(std::launch::async, [&] {
        return fakeHttp(l2, 422, "{\"ErrorCode\":300,\"Message\":\"Invalid 'To' address\"}");
    });
    settings.apiUrl = "http://127.0.0.1:" + std::to_string(l2.port) + "/email";
    sent = makePostmarkMailer(settings)->send(sample());
    refusing.get();
    REQUIRE_FALSE(sent);
    CHECK(contains(sent.error().message, "Invalid 'To' address"));
}

TEST_CASE("mail: the dev log") {
    std::string out;
    auto mailer = makeLogMailer([&](const std::string& text) { out = text; });
    REQUIRE(mailer->send(sample()));
    CHECK(contains(out, "To: ann@example.com"));
    CHECK(contains(out, "token=abc"));
    MailMessage bad = sample();
    bad.to = "nope";
    CHECK_FALSE(mailer->send(bad));
}
