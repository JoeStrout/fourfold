// Mailer: sending email (Arch §5 mail; overview §5, Sign-in methods).
//
// Messages are structured: an address, a subject and a body, never raw
// headers, so nothing a visitor types can add a header or a recipient (PHP's
// mail() header injection). The transports are SMTP (with STARTTLS or
// implicit TLS) and Postmark's HTTP API, both over libcurl; and, for dev
// mode, a log that prints the message instead of sending it.
//
// send() blocks on the network, so it's for worker threads. Credentials come
// in the settings, resolved from secrets.ms by `config`; they never reach
// the script thread.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/result.h"

namespace fourfold {

struct MailMessage {
    std::string from;      // "noreply@example.com", or "Name <noreply@example.com>"
    std::string to;        // one address
    std::string subject;
    std::string text;      // the plain-text body
    std::optional<std::string> html;   // an HTML alternative, if any
};

// A plain address: local@domain, printable ASCII, no spaces, quotes, angle
// brackets or line breaks, at most 254 bytes.
bool isValidEmailAddress(std::string_view address);

// Checks a message before it's sent: valid addresses (`from` may have a
// display name), and no line breaks or control characters in the subject.
Status validateMessage(const MailMessage& message);

class Mailer {
public:
    virtual ~Mailer() = default;
    // Blocks. Invalid if the message fails validateMessage.
    virtual Status send(const MailMessage& message) = 0;
};

struct SmtpSettings {
    std::string host;
    int port = 587;
    enum class Security {
        StartTls,   // plain connection, upgraded with STARTTLS (required) (usually port 587)
        Tls,        // TLS from the start (usually port 465)
        None,       // no TLS: only for a relay on this machine (localhost)
    };
    Security security = Security::StartTls;
    std::string username;   // empty: no authentication
    std::string password;
    std::chrono::milliseconds timeout{30'000};
};

struct PostmarkSettings {
    std::string serverToken;
    std::string messageStream = "outbound";
    std::string apiUrl = "https://api.postmarkapp.com/email";   // tests point it elsewhere
    std::chrono::milliseconds timeout{30'000};
};

std::unique_ptr<Mailer> makeSmtpMailer(SmtpSettings settings);
std::unique_ptr<Mailer> makePostmarkMailer(PostmarkSettings settings);
// Dev mode: hands each message, as readable text, to `out` (the terminal and
// the engine log), and sends nothing.
std::unique_ptr<Mailer> makeLogMailer(std::function<void(const std::string&)> out);

// The message as sent over SMTP: headers, then the body as quoted-printable
// (multipart/alternative with an HTML part), with CRLF line ends.
// `date` is RFC 5322's, e.g. "Tue, 27 Oct 2026 14:05:00 +0000".
std::string formatMessage(const MailMessage& message, const std::string& date, const std::string& messageId);

}  // namespace fourfold
