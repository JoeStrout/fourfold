#include "pipeline/forms.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "base/utf8.h"
#include "pipeline/stages.h"

namespace fourfold {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// The engine's own fields come out of the submission.
void takeField(Submission& s, std::string name, std::string value) {
    std::optional<std::string>* special = name == "_csrf" ? &s.csrf : name == "_t" ? &s.formTime
                                          : name == "_pow" ? &s.challenge
                                                           : nullptr;
    if (special) {
        if (!*special) *special = std::move(value);
        return;
    }
    s.fields.emplace_back(std::move(name), std::move(value));
}

// A header parameter's value: `name=value` or `name="value"` in "a; b=c;
// name=..." (case-insensitive name). Browsers write multipart names and
// file names as the HTML spec says: `"`, CR and LF as %22, %0D and %0A, and
// nothing else escaped (a backslash is just a backslash, as in a Windows
// path), so that's how they're read.
std::optional<std::string> headerParameter(std::string_view header, std::string_view name) {
    size_t i = header.find(';');
    while (i != std::string_view::npos && i < header.size()) {
        i++;
        while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) i++;
        size_t eq = header.find('=', i);
        if (eq == std::string_view::npos) return std::nullopt;
        std::string key = lower(trim(header.substr(i, eq - i)));
        i = eq + 1;
        std::string value;
        if (i < header.size() && header[i] == '"') {
            i++;
            while (i < header.size() && header[i] != '"') value += header[i++];
            for (const auto& [code, c] : {std::pair<const char*, char>{"%22", '"'}, {"%0D", '\r'}, {"%0A", '\n'}}) {
                for (size_t at = value.find(code); at != std::string::npos; at = value.find(code, at + 1)) {
                    value.replace(at, 3, 1, c);
                }
            }
            i = header.find(';', i);
        } else {
            size_t end = header.find(';', i);
            value = std::string(trim(header.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i)));
            i = end;
        }
        if (key == name) return value;
    }
    return std::nullopt;
}

std::optional<Submission> readMultipart(const HttpRequest& request, const std::string& contentType,
                                        const SubmissionLimits& limits, SubmissionError& error) {
    auto fail = [&](int status, std::string message) {
        error = {status, std::move(message)};
        return std::nullopt;
    };
    std::optional<std::string> boundary = headerParameter(contentType, "boundary");
    if (!boundary || boundary->empty() || boundary->size() > 70) return fail(400, "multipart body with no boundary");
    const std::string& body = request.body;
    std::string delimiter = "--" + *boundary;
    // The first delimiter, then parts until the closing one.
    size_t at = body.find(delimiter);
    if (at == std::string::npos) return fail(400, "multipart body with no parts");
    Submission s;
    size_t parts = 0, formBytes = 0;
    std::string next = "\r\n" + delimiter;
    at += delimiter.size();
    while (true) {
        if (body.compare(at, 2, "--") == 0) break;   // the closing delimiter
        if (body.compare(at, 2, "\r\n") != 0) return fail(400, "malformed multipart body");
        at += 2;
        if (++parts > limits.maxParts) {
            return fail(413, "more than " + std::to_string(limits.maxParts) + " parts in the form");
        }
        size_t headersEnd = body.find("\r\n\r\n", at);
        if (headersEnd == std::string::npos || headersEnd - at > limits.maxPartHeaderBytes) {
            return fail(400, "malformed multipart part headers");
        }
        std::string disposition;
        size_t line = at;
        while (line < headersEnd) {
            size_t end = body.find("\r\n", line);
            if (end == std::string::npos || end > headersEnd) end = headersEnd;
            std::string_view header(body.data() + line, end - line);
            size_t colon = header.find(':');
            if (colon != std::string_view::npos && lower(trim(header.substr(0, colon))) == "content-disposition") {
                disposition = std::string(trim(header.substr(colon + 1)));
            }
            line = end + 2;
        }
        size_t contentStart = headersEnd + 4;
        size_t contentEnd = body.find(next, contentStart);
        if (contentEnd == std::string::npos) return fail(400, "multipart body ends in the middle of a part");
        at = contentEnd + next.size();

        if (lower(disposition.substr(0, disposition.find(';'))) != "form-data") continue;
        std::optional<std::string> name = headerParameter(disposition, "name");
        if (!name) continue;
        std::optional<std::string> filename = headerParameter(disposition, "filename");
        std::string content = body.substr(contentStart, contentEnd - contentStart);
        if (filename) {
            s.uploads.push_back({toValidUtf8(*name), toValidUtf8(*filename),
                                 std::make_shared<const std::string>(std::move(content))});
            continue;
        }
        formBytes += content.size();
        if (formBytes > limits.maxFormBytes) return fail(413, "the form's fields are too large");
        takeField(s, toValidUtf8(*name), toValidUtf8(content));
    }
    return s;
}

}  // namespace

std::optional<Submission> readSubmission(const HttpRequest& request, const SubmissionLimits& limits,
                                         SubmissionError& error) {
    if (request.method != "POST") return Submission{};
    const std::string* typeHeader = request.headers.get("Content-Type");
    std::string type = typeHeader ? *typeHeader : "";
    std::string media = lower(trim(type.substr(0, type.find(';'))));
    if (media == "application/x-www-form-urlencoded") {
        if (request.body.size() > limits.maxFormBytes) {
            error = {413, "the form is too large"};
            return std::nullopt;
        }
        Submission s;
        RequestFacts::Pairs pairs = parseUrlEncoded(request.body);
        if (pairs.size() > limits.maxFields) {
            error = {413, "more than " + std::to_string(limits.maxFields) + " fields in the form"};
            return std::nullopt;
        }
        for (auto& [name, value] : pairs) takeField(s, std::move(name), std::move(value));
        return s;
    }
    if (media == "multipart/form-data") return readMultipart(request, type, limits, error);
    return Submission{};
}

std::string detectContentType(std::string_view b) {
    auto starts = [&](std::string_view prefix) { return b.substr(0, prefix.size()) == prefix; };
    if (starts(std::string_view("\x89PNG\r\n\x1a\n", 8))) return "image/png";
    if (starts(std::string_view("\xFF\xD8\xFF", 3))) return "image/jpeg";
    if (starts("GIF87a") || starts("GIF89a")) return "image/gif";
    if (b.size() >= 12 && starts("RIFF") && b.substr(8, 4) == "WEBP") return "image/webp";
    if (starts("%PDF-")) return "application/pdf";
    if (starts(std::string_view("PK\x03\x04", 4)) || starts(std::string_view("PK\x05\x06", 4))) {
        return "application/zip";
    }
    // Text: valid UTF-8, without NUL or the other control characters that
    // mark binary data (tab, newlines and form feed are fine).
    if (!isValidUtf8(b)) return "application/octet-stream";
    for (unsigned char c : b) {
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || c == 0x7F) {
            return "application/octet-stream";
        }
    }
    return "text/plain";
}

std::string normalizeFormText(std::string_view text, bool multiline) {
    std::string nfc = toNfc(text);
    std::string out;
    out.reserve(nfc.size());
    for (size_t i = 0; i < nfc.size(); i++) {
        unsigned char c = static_cast<unsigned char>(nfc[i]);
        if (c == '\r') {
            if (multiline) out += '\n';
            if (i + 1 < nfc.size() && nfc[i + 1] == '\n') i++;
            continue;
        }
        if (c == '\n' || c == '\t') {
            if (multiline) out += static_cast<char>(c);
            else out += ' ';
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        // C1 controls (U+0080–U+009F), as UTF-8: C2 80–C2 9F.
        if (c == 0xC2 && i + 1 < nfc.size() && static_cast<unsigned char>(nfc[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(nfc[i + 1]) <= 0x9F) {
            i++;
            continue;
        }
        out += static_cast<char>(c);
    }
    // Trim whitespace (ASCII, and the no-break space U+00A0) from both ends.
    auto spaceAt = [&](size_t at) -> size_t {   // its length, or 0
        char c = out[at];
        if (c == ' ' || c == '\t' || c == '\n') return 1;
        if (out.compare(at, 2, "\xC2\xA0") == 0) return 2;
        return 0;
    };
    size_t start = 0;
    while (start < out.size() && spaceAt(start)) start += spaceAt(start);
    size_t end = out.size();
    while (end > start) {
        char c = out[end - 1];
        if (c == ' ' || c == '\t' || c == '\n') {
            end--;
        } else if (end - start >= 2 && out.compare(end - 2, 2, "\xC2\xA0") == 0) {
            end -= 2;
        } else {
            break;
        }
    }
    return out.substr(start, end - start);
}

size_t countLinks(std::string_view text) {
    std::string l = lower(text);
    size_t n = 0;
    for (const char* marker : {"http://", "https://", "www."}) {
        for (size_t at = l.find(marker); at != std::string::npos; at = l.find(marker, at + 1)) {
            // "http://www.x" is one link, not two.
            if (std::string(marker) == "www." && at >= 3 && l.compare(at - 3, 3, "://") == 0) continue;
            n++;
        }
    }
    return n;
}

namespace {

std::string show(double n) {
    if (n == std::floor(n) && std::fabs(n) < 1e15) return std::to_string(static_cast<long long>(n));
    std::string s = std::to_string(n);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool isValidEmail(const std::string& s) {
    if (s.size() > 254) return false;
    size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at > 64 || s.find('@', at + 1) != std::string::npos) return false;
    for (size_t i = 0; i < at; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= ' ' || c == 0x7F || c == '"' || c == '(' || c == ')' || c == ',' || c == ':' || c == ';' ||
            c == '<' || c == '>' || c == '[' || c == ']' || c == '\\') {
            return false;
        }
    }
    std::string domain = s.substr(at + 1);
    if (domain.empty() || domain.find('.') == std::string::npos || domain.front() == '.' || domain.back() == '.' ||
        domain.find("..") != std::string::npos) {
        return false;
    }
    for (unsigned char c : domain) {
        if (!(std::isalnum(c) || c == '-' || c == '.' || c >= 0x80)) return false;
    }
    return true;
}

bool isValidHttpUrl(const std::string& s) {
    std::string l = lower(s);
    size_t rest = l.compare(0, 7, "http://") == 0 ? 7 : l.compare(0, 8, "https://") == 0 ? 8 : 0;
    if (!rest || rest >= s.size()) return false;
    if (s[rest] == '/' || s[rest] == '?' || s[rest] == '#') return false;   // no host
    for (unsigned char c : s) {
        if (c <= ' ' || c == 0x7F || c == '\\' || c == '"' || c == '<' || c == '>') return false;
    }
    return true;
}

// Strict decimal: -?digits(.digits)?(e[+-]?digits)?, finite, and (for int)
// whole and within ±2^53.
std::optional<double> parseNumber(const std::string& s, bool integer) {
    size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    size_t digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++, digits++;
    if (!integer && i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++, digits++;
    }
    if (!digits) return std::nullopt;
    if (!integer && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        size_t exp = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++, exp++;
        if (!exp) return std::nullopt;
    }
    if (i != s.size() || s.size() > 64) return std::nullopt;
    double v = std::strtod(s.c_str(), nullptr);
    if (!std::isfinite(v)) return std::nullopt;
    if (integer && std::fabs(v) > 9007199254740992.0) return std::nullopt;
    if (v == 0) v = 0;   // no -0
    return v;
}

bool isValidDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    int y = std::atoi(s.substr(0, 4).c_str()), m = std::atoi(s.substr(5, 2).c_str()),
        d = std::atoi(s.substr(8, 2).c_str());
    if (y < 1 || m < 1 || m > 12 || d < 1) return false;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= kDays[m - 1] + (m == 2 && leap ? 1 : 0);
}

std::string typeNames(const std::vector<std::string>& types) {
    std::string out;
    for (size_t i = 0; i < types.size(); i++) {
        std::string t = types[i];
        std::string name = t == "image/png" ? "PNG" : t == "image/jpeg" ? "JPEG" : t == "image/gif" ? "GIF"
                         : t == "image/webp" ? "WebP" : t == "image/*" ? "an image" : t == "application/pdf" ? "PDF"
                         : t == "application/zip" ? "ZIP" : t == "text/plain" ? "plain text" : t;
        if (i) out += i + 1 == types.size() ? " or " : ", ";
        out += name;
    }
    return out;
}

bool typeAllowed(const std::vector<std::string>& types, const std::string& detected) {
    for (const std::string& t : types) {
        if (t == detected) return true;
        if (t == "image/*" && (detected == "image/png" || detected == "image/jpeg" || detected == "image/gif" ||
                               detected == "image/webp")) {
            return true;
        }
    }
    return false;
}

// The file name as sent, cleaned: no folders, no control characters.
std::string cleanFilename(const std::string& name) {
    size_t slash = name.find_last_of("/\\");
    std::string base = normalizeFormText(slash == std::string::npos ? name : name.substr(slash + 1), false);
    if (base.size() > 255) base.resize(255);
    while (!isValidUtf8(base)) base.pop_back();
    return base;
}

std::string defaultMessage(const FormField& f, const std::string& rule) {
    if (rule == "required") {
        switch (f.type) {
            case FormFieldType::Bool: return "Please check this box.";
            case FormFieldType::File: return "Please choose a file.";
            case FormFieldType::Choice: return "Please choose one.";
            case FormFieldType::Choices: return "Please choose at least one.";
            default: return "Please fill this in.";
        }
    }
    if (rule == "type") {
        switch (f.type) {
            case FormFieldType::Text: return "Please keep this to one line.";
            case FormFieldType::Email: return "Please enter an email address.";
            case FormFieldType::Url: return "Please enter a web address starting with http:// or https://.";
            case FormFieldType::Number: return "Please enter a number.";
            case FormFieldType::Int: return "Please enter a whole number.";
            case FormFieldType::Date: return "Please enter a date (YYYY-MM-DD).";
            default: return "Please check this.";
        }
    }
    bool number = f.type == FormFieldType::Number || f.type == FormFieldType::Int;
    if (rule == "min") {
        if (number) return "Please enter " + show(*f.min) + " or more.";
        return "Please use at least " + show(*f.min) + (*f.min == 1 ? " character." : " characters.");
    }
    if (rule == "max") {
        if (number) return "Please enter " + show(*f.max) + " or less.";
        return "Please use at most " + show(*f.max) + (*f.max == 1 ? " character." : " characters.");
    }
    if (rule == "maxLinks") {
        if (*f.maxLinks == 0) return "Please don't include links.";
        return "Please include no more than " + std::to_string(*f.maxLinks) + (*f.maxLinks == 1 ? " link." : " links.");
    }
    if (rule == "maxKB") return "Please choose a file no bigger than " + show(f.maxKB) + " KB.";
    if (rule == "types") return "Please choose a file of the right type (" + typeNames(f.types) + ").";
    return "Please check this.";
}

std::string messageFor(const FormField& f, const std::string& rule) {
    auto it = f.messages.find(rule);
    return it != f.messages.end() ? it->second : defaultMessage(f, rule);
}

}  // namespace

CheckedForm checkForm(const FormSpec& spec, const Submission& submission) {
    Datum::MapItems form, errors, typed;
    std::vector<UploadFacts> files;
    auto values = [&](const std::string& name) {
        std::vector<const std::string*> out;
        for (const auto& [n, v] : submission.fields) {
            if (n == name) out.push_back(&v);
        }
        return out;
    };
    for (const FormField& f : spec.fields) {
        auto error = [&](const std::string& rule) { errors[f.name] = Datum::string(messageFor(f, rule)); };
        if (f.type == FormFieldType::File) {
            const RawUpload* upload = nullptr;
            for (const RawUpload& u : submission.uploads) {
                if (u.field == f.name && u.bytes && (!u.bytes->empty() || !u.filename.empty())) {
                    upload = &u;
                    break;
                }
            }
            if (!upload || upload->bytes->empty()) {
                if (f.required) error("required");
                continue;
            }
            if (static_cast<double>(upload->bytes->size()) > f.maxKB * 1024) {
                error("maxKB");
                continue;
            }
            std::string detected = detectContentType(*upload->bytes);
            if (!typeAllowed(f.types, detected)) {
                error("types");
                continue;
            }
            files.push_back({f.name, cleanFilename(upload->filename), detected, upload->bytes});
            continue;
        }

        std::vector<const std::string*> given = values(f.name);
        if (f.type == FormFieldType::Bool) {
            bool on = !given.empty();
            form[f.name] = Datum::number(on ? 1 : 0);
            typed[f.name] = Datum::string(on ? "1" : "");
            if (f.required && !on) error("required");
            continue;
        }
        if (f.type == FormFieldType::Choices) {
            Datum::ListItems chosen;
            for (const std::string& o : f.options) {
                bool picked = false;
                for (const std::string* v : given) picked = picked || normalizeFormText(*v, false) == o;
                if (picked) chosen.push_back(Datum::string(o));
            }
            if (f.required && chosen.empty()) error("required");
            form[f.name] = Datum::list(chosen);
            typed[f.name] = Datum::list(chosen);
            continue;
        }
        std::string raw = given.empty() ? "" : *given[0];
        if (f.type == FormFieldType::Choice) {
            std::string v = normalizeFormText(raw, false);
            bool valid = std::find(f.options.begin(), f.options.end(), v) != f.options.end();
            // A value not in the options is dropped, like an undeclared field.
            form[f.name] = valid ? Datum::string(v) : Datum();
            typed[f.name] = Datum::string(valid ? v : "");
            if (!valid && f.required) error("required");
            continue;
        }

        // The text types (and numbers and dates, which arrive as text).
        bool multiline = f.type == FormFieldType::Multiline;
        bool newline = !multiline && raw.find_first_of("\r\n") != std::string::npos;
        std::string v = normalizeFormText(raw, multiline);
        typed[f.name] = Datum::string(v);
        bool textual = f.type == FormFieldType::Text || multiline || f.type == FormFieldType::Email ||
                       f.type == FormFieldType::Url;
        if (v.empty()) {
            form[f.name] = textual ? Datum::string("") : Datum();
            if (f.required) error("required");
            continue;
        }
        if (f.type == FormFieldType::Text && newline) {
            form[f.name] = Datum();
            error("type");
            continue;
        }
        if ((f.type == FormFieldType::Email && !isValidEmail(v)) || (f.type == FormFieldType::Url && !isValidHttpUrl(v)) ||
            (f.type == FormFieldType::Date && !isValidDate(v))) {
            form[f.name] = Datum();
            error("type");
            continue;
        }
        if (f.type == FormFieldType::Number || f.type == FormFieldType::Int) {
            std::optional<double> n = parseNumber(v, f.type == FormFieldType::Int);
            if (!n) {
                form[f.name] = Datum();
                error("type");
                continue;
            }
            if ((f.min && *n < *f.min) || (f.max && *n > *f.max)) {
                form[f.name] = Datum();
                error(f.min && *n < *f.min ? "min" : "max");
                continue;
            }
            form[f.name] = Datum::number(*n);
            continue;
        }
        size_t length = utf8Length(v);
        if (f.min && static_cast<double>(length) < *f.min) {
            form[f.name] = Datum();
            error("min");
            continue;
        }
        if (f.max && static_cast<double>(length) > *f.max) {
            form[f.name] = Datum();
            error("max");
            continue;
        }
        if (f.maxLinks && countLinks(v) > static_cast<size_t>(*f.maxLinks)) {
            form[f.name] = Datum();
            error("maxLinks");
            continue;
        }
        form[f.name] = Datum::string(v);
    }
    CheckedForm out;
    out.form = Datum::map(std::move(form));
    if (!errors.empty()) out.errors = Datum::map(std::move(errors));
    out.files = std::move(files);
    out.typed = Datum::map(std::move(typed));
    return out;
}

}  // namespace fourfold
