#include "compiledapp/outbound_policy.h"

#include <cctype>

namespace fourfold {

namespace {

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

bool isValidHostName(const std::string& host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) dot = host.size();
        std::string label = host.substr(start, dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (char c : label) {
            if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
                  c == '-')) {
                return false;
            }
        }
        start = dot + 1;
    }
    return true;
}

std::optional<HostRule> parseHostRule(const std::string& text, std::string& error) {
    HostRule rule;
    rule.text = text;
    std::string s = lowered(text);
    if (s.compare(0, 7, "http://") == 0) {
        rule.http = true;
        s = s.substr(7);
    } else if (s.compare(0, 8, "https://") == 0) {
        s = s.substr(8);
    }
    if (!s.empty() && s.back() == '/') s.pop_back();
    if (s.find_first_of("/?#@ ") != std::string::npos) {
        error = "\"" + text + "\" isn't a host: write only the host name, like \"api.example.com\" (no path)";
        return std::nullopt;
    }
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        std::string port = s.substr(colon + 1);
        if (!isDigits(port) || port.size() > 5 || std::stoi(port) < 1 || std::stoi(port) > 65535) {
            error = "\"" + text + "\" has a port that isn't a number from 1 to 65535";
            return std::nullopt;
        }
        rule.port = std::stoi(port);
        s.resize(colon);
    }
    if (s == "*") {
        rule.any = true;
        return rule;
    }
    if (s.compare(0, 2, "*.") == 0) {
        rule.subdomains = true;
        s = s.substr(2);
    }
    if (s.find('*') != std::string::npos) {
        error = "\"" + text + "\": a * can only be the whole rule (\"*\"), or start one (\"*.example.com\")";
        return std::nullopt;
    }
    if (!isValidHostName(s)) {
        error = "\"" + text + "\" isn't a host name (international names are written in their xn-- form)";
        return std::nullopt;
    }
    if (rule.subdomains && s.find('.') == std::string::npos) {
        error = "\"" + text + "\" would allow every name under a top-level domain; name the site's own domain";
        return std::nullopt;
    }
    rule.host = s;
    return rule;
}

bool HostRule::allows(const std::string& h, int p, bool https) const {
    if (!https && !http) return false;
    if (port ? p != *port : p != (https ? 443 : 80)) return false;
    if (any) return true;
    if (subdomains) {
        return h.size() > host.size() + 1 && h.compare(h.size() - host.size(), host.size(), host) == 0 &&
               h[h.size() - host.size() - 1] == '.';
    }
    return h == host;
}

bool OutboundPolicy::allowsHost(const std::string& host, int port, bool https) const {
    for (const HostRule& r : hosts) {
        if (r.allows(host, port, https)) return true;
    }
    return false;
}

bool OutboundPolicy::anyHost() const {
    for (const HostRule& r : hosts) {
        if (r.any) return true;
    }
    return false;
}

std::optional<std::string> parseMailRule(const std::string& text, std::string& error) {
    std::string s = lowered(text);
    if (s == "*") return s;
    size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= s.size() || s.find('@', at + 1) != std::string::npos) {
        error = "\"" + text + "\" isn't an email address (or \"*@example.com\" for a whole domain, or \"*\")";
        return std::nullopt;
    }
    std::string local = s.substr(0, at);
    std::string domain = s.substr(at + 1);
    if (!isValidHostName(domain) || domain.find('.') == std::string::npos) {
        error = "\"" + text + "\" has a domain that isn't a host name";
        return std::nullopt;
    }
    if (local == "*") return s;
    for (unsigned char c : local) {
        if (c <= 0x20 || c >= 0x7F || c == '<' || c == '>' || c == '"' || c == '(' || c == ')' || c == ',' ||
            c == ';' || c == ':' || c == '\\' || c == '[' || c == ']' || c == '*') {
            error = "\"" + text + "\" isn't a plain email address";
            return std::nullopt;
        }
    }
    return s;
}

bool OutboundPolicy::allowsMailTo(const std::string& address) const {
    size_t at = address.rfind('@');
    for (const std::string& rule : mailTo) {
        if (rule == "*" || rule == address) return true;
        if (rule.compare(0, 2, "*@") == 0 && at != std::string::npos && address.substr(at + 1) == rule.substr(2)) {
            return true;
        }
    }
    return false;
}

bool OutboundPolicy::anyMail() const {
    for (const std::string& rule : mailTo) {
        if (rule == "*") return true;
    }
    return false;
}

}  // namespace fourfold
