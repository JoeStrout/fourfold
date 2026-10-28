#include "mail/mailer.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>

#include "base/crypto.h"
#include "base/datum.h"
#include "platform/random.h"

namespace fourfold {

namespace {

constexpr size_t kMaxAddress = 254;
constexpr size_t kMaxSubject = 998;
constexpr size_t kMaxBody = 1024 * 1024;
constexpr size_t kQpLine = 76;

bool isAtext(unsigned char c) {
    return c > 0x20 && c < 0x7F && !std::strchr("\"(),:;<>[\\]", c);
}

// "Name <a@b.c>" → {"Name", "a@b.c"}; "a@b.c" → {"", "a@b.c"}.
std::pair<std::string, std::string> splitMailbox(const std::string& mailbox) {
    if (!mailbox.empty() && mailbox.back() == '>') {
        size_t open = mailbox.rfind('<');
        if (open != std::string::npos) {
            std::string name = mailbox.substr(0, open);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            return {name, mailbox.substr(open + 1, mailbox.size() - open - 2)};
        }
    }
    return {"", mailbox};
}

bool hasControl(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

bool isAscii(std::string_view s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
}

std::string base64(std::string_view bytes) {
    static const char* const kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        uint32_t n = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        for (int shift : {18, 12, 6, 0}) out += kDigits[(n >> shift) & 63];
    }
    if (i < bytes.size()) {
        uint32_t n = uint8_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= uint8_t(bytes[i + 1]) << 8;
        out += kDigits[(n >> 18) & 63];
        out += kDigits[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kDigits[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// A header's text: as is if it's ASCII, else RFC 2047's encoded word.
std::string headerText(const std::string& text) {
    if (isAscii(text)) return text;
    return "=?UTF-8?B?" + base64(text) + "?=";
}

// A display name, quoted if it needs to be.
std::string displayName(const std::string& name) {
    if (!isAscii(name)) return headerText(name);
    bool plain = true;
    for (unsigned char c : name) plain = plain && (isAtext(c) || c == ' ');
    if (plain) return name;
    std::string quoted = "\"";
    for (char c : name) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}

// The body as quoted-printable, with CRLF line ends. A line's leading '.'
// is encoded too, so SMTP's end-of-data marker can't appear.
std::string quotedPrintable(const std::string& text) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        size_t column = 0;
        for (size_t i = 0; i < line.size(); i++) {
            unsigned char c = static_cast<unsigned char>(line[i]);
            bool last = i + 1 == line.size();
            std::string piece;
            if ((c >= 33 && c <= 126 && c != '=' && !(c == '.' && column == 0)) ||
                ((c == ' ' || c == '\t') && !last)) {
                piece = std::string(1, static_cast<char>(c));
            } else {
                piece = {'=', kHex[c >> 4], kHex[c & 15]};
            }
            if (column + piece.size() > kQpLine - 1) {
                out += "=\r\n";
                column = 0;
                if (piece == ".") piece = "=2E";
            }
            out += piece;
            column += piece.size();
        }
        out += "\r\n";
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

std::string rfc5322Date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[64];
    std::strftime(buffer, sizeof buffer, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return buffer;
}

std::string domainOf(const std::string& address) {
    size_t at = address.rfind('@');
    return at == std::string::npos ? "localhost" : address.substr(at + 1);
}

void initCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Limits a handle to the given protocols ("smtp,smtps", "https").
void restrictProtocols(CURL* curl, const char* protocols) {
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else
    long mask = std::strstr(protocols, "smtp") ? (CURLPROTO_SMTP | CURLPROTO_SMTPS) : CURLPROTO_HTTPS | CURLPROTO_HTTP;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, mask);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, mask);
#endif
}

struct Upload {
    std::string data;
    size_t sent = 0;
};

size_t readUpload(char* buffer, size_t size, size_t count, void* userdata) {
    auto* upload = static_cast<Upload*>(userdata);
    size_t n = std::min(size * count, upload->data.size() - upload->sent);
    std::memcpy(buffer, upload->data.data() + upload->sent, n);
    upload->sent += n;
    return n;
}

size_t collect(char* data, size_t size, size_t count, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t n = size * count;
    if (out->size() + n > 64 * 1024) return 0;   // a response this big isn't Postmark's
    out->append(data, n);
    return n;
}

class SmtpMailer : public Mailer {
public:
    explicit SmtpMailer(SmtpSettings s) : settings_(std::move(s)) { initCurl(); }

    Status send(const MailMessage& message) override {
        if (Status s = validateMessage(message); !s) return s;
        std::string from = splitMailbox(message.from).second;
        std::string id = toHex(randomBytes(16)) + "@" + domainOf(from);
        Upload upload{formatMessage(message, rfc5322Date(), id), 0};

        CURL* curl = curl_easy_init();
        if (!curl) return makeError(ErrorCode::IO, "couldn't start sending mail");
        bool tls = settings_.security == SmtpSettings::Security::Tls;
        std::string url = std::string(tls ? "smtps://" : "smtp://") + settings_.host + ":" +
                          std::to_string(settings_.port);
        curl_slist* recipients = curl_slist_append(nullptr, ("<" + message.to + ">").c_str());
        std::string mailFrom = "<" + from + ">";
        char error[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        restrictProtocols(curl, "smtp,smtps");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(settings_.timeout.count()));
        if (settings_.security == SmtpSettings::Security::StartTls) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        }
        if (!settings_.username.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, settings_.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, settings_.password.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mailFrom.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, readUpload);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        CURLcode result = curl_easy_perform(curl);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            return makeError(ErrorCode::IO, "sending mail through " + settings_.host + " failed: " +
                                                (error[0] ? error : curl_easy_strerror(result)));
        }
        return {};
    }

private:
    SmtpSettings settings_;
};

class PostmarkMailer : public Mailer {
public:
    explicit PostmarkMailer(PostmarkSettings s) : settings_(std::move(s)) { initCurl(); }

    Status send(const MailMessage& message) override {
        if (Status s = validateMessage(message); !s) return s;
        Datum::MapItems fields{{"From", Datum::string(message.from)},
                               {"To", Datum::string(message.to)},
                               {"Subject", Datum::string(message.subject)},
                               {"TextBody", Datum::string(message.text)},
                               {"MessageStream", Datum::string(settings_.messageStream)}};
        if (message.html) fields["HtmlBody"] = Datum::string(*message.html);
        std::string body = toJson(Datum::map(std::move(fields)));
        std::string response;

        CURL* curl = curl_easy_init();
        if (!curl) return makeError(ErrorCode::IO, "couldn't start sending mail");
        curl_slist* headers = nullptr;
        std::string tokenHeader = "X-Postmark-Server-Token: " + settings_.serverToken;
        for (const char* h : {"Accept: application/json", "Content-Type: application/json"}) {
            headers = curl_slist_append(headers, h);
        }
        headers = curl_slist_append(headers, tokenHeader.c_str());
        char error[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_URL, settings_.apiUrl.c_str());
        restrictProtocols(curl, "https,http");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(settings_.timeout.count()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            return makeError(ErrorCode::IO, std::string("sending mail through Postmark failed: ") +
                                                (error[0] ? error : curl_easy_strerror(result)));
        }
        if (status != 200) {
            std::string why = "HTTP status " + std::to_string(status);
            if (auto parsed = parseJson(response); parsed && parsed.value().kind() == Datum::Kind::Map) {
                auto m = parsed.value().asMap().find("Message");
                if (m != parsed.value().asMap().end() && m->second.kind() == Datum::Kind::String) {
                    why += ": " + m->second.asString();
                }
            }
            return makeError(ErrorCode::IO, "Postmark refused the message (" + why + ")");
        }
        return {};
    }

private:
    PostmarkSettings settings_;
};

class LogMailer : public Mailer {
public:
    explicit LogMailer(std::function<void(const std::string&)> out) : out_(std::move(out)) {}

    Status send(const MailMessage& message) override {
        if (Status s = validateMessage(message); !s) return s;
        if (out_) {
            out_("Mail (not sent; dev mode prints it instead)\nFrom: " + message.from + "\nTo: " + message.to +
                 "\nSubject: " + message.subject + "\n\n" + message.text);
        }
        return {};
    }

private:
    std::function<void(const std::string&)> out_;
};

}  // namespace

bool isValidEmailAddress(std::string_view address) {
    if (address.empty() || address.size() > kMaxAddress) return false;
    size_t at = address.rfind('@');
    if (at == std::string_view::npos || at == 0 || at + 1 == address.size()) return false;
    std::string_view local = address.substr(0, at), domain = address.substr(at + 1);
    for (unsigned char c : local) {
        if (!isAtext(c) || c == '@') return false;
    }
    if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string_view::npos) return false;
    if (domain.find('.') == std::string_view::npos || domain.front() == '.' || domain.back() == '.' ||
        domain.find("..") != std::string_view::npos) {
        return false;
    }
    for (unsigned char c : domain) {
        if (!(std::isalnum(c) || c == '-' || c == '.')) return false;
    }
    return true;
}

Status validateMessage(const MailMessage& message) {
    auto [name, from] = splitMailbox(message.from);
    if (!isValidEmailAddress(from) || hasControl(name) || name.find_first_of("<>") != std::string::npos) {
        return makeError(ErrorCode::Invalid, "the sender's address isn't valid: " + message.from);
    }
    if (!isValidEmailAddress(message.to)) {
        return makeError(ErrorCode::Invalid, "the recipient's address isn't valid: " + message.to);
    }
    if (hasControl(message.subject) || message.subject.size() > kMaxSubject) {
        return makeError(ErrorCode::Invalid, "the subject has a line break or control character, or is too long");
    }
    if (message.text.size() > kMaxBody || (message.html && message.html->size() > kMaxBody)) {
        return makeError(ErrorCode::TooLarge, "the message is too long");
    }
    return {};
}

std::string formatMessage(const MailMessage& message, const std::string& date, const std::string& messageId) {
    auto [name, from] = splitMailbox(message.from);
    std::string out = "Date: " + date + "\r\n";
    out += "From: " + (name.empty() ? from : displayName(name) + " <" + from + ">") + "\r\n";
    out += "To: <" + message.to + ">\r\n";
    out += "Subject: " + headerText(message.subject) + "\r\n";
    out += "Message-ID: <" + messageId + ">\r\n";
    out += "MIME-Version: 1.0\r\n";
    const char* textHeaders = "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n";
    if (!message.html) {
        out += textHeaders;
        out += "\r\n" + quotedPrintable(message.text);
        return out;
    }
    std::string boundary = "ff-" + toHex(hashBytes(messageId)).substr(0, 24);
    out += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
    out += "--" + boundary + "\r\n" + textHeaders + "\r\n" + quotedPrintable(message.text);
    out += "--" + boundary + "\r\nContent-Type: text/html; charset=utf-8\r\n"
           "Content-Transfer-Encoding: quoted-printable\r\n\r\n" + quotedPrintable(*message.html);
    out += "--" + boundary + "--\r\n";
    return out;
}

std::unique_ptr<Mailer> makeSmtpMailer(SmtpSettings settings) {
    return std::make_unique<SmtpMailer>(std::move(settings));
}

std::unique_ptr<Mailer> makePostmarkMailer(PostmarkSettings settings) {
    return std::make_unique<PostmarkMailer>(std::move(settings));
}

std::unique_ptr<Mailer> makeLogMailer(std::function<void(const std::string&)> out) {
    return std::make_unique<LogMailer>(std::move(out));
}

}  // namespace fourfold
