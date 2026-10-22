// HttpRequest and HttpResponse: the seam between transports (the standalone
// server, CGI, FastCGI, the test runner) and the engine (Arch §6). Plain
// structs, so the engine can't tell which transport a request came from.
#pragma once

#include <string>
#include <vector>

namespace fourfold {

// An ordered list of header fields. Names compare case-insensitively.
class Headers {
public:
    struct Field {
        std::string name;
        std::string value;
    };

    // Appends a field, keeping any others of the same name.
    void add(std::string name, std::string value);
    // Replaces all fields of this name with one.
    void set(const std::string& name, std::string value);
    void remove(const std::string& name);

    // The first field of this name, or nullptr.
    const std::string* get(const std::string& name) const;
    bool has(const std::string& name) const { return get(name) != nullptr; }

    size_t size() const { return fields_.size(); }
    std::vector<Field>::const_iterator begin() const { return fields_.begin(); }
    std::vector<Field>::const_iterator end() const { return fields_.end(); }

private:
    std::vector<Field> fields_;
};

bool equalsIgnoreCase(const std::string& a, const std::string& b);

struct HttpRequest {
    std::string method;          // "GET", "POST", …
    std::string target;          // as sent: "/path?query" (not yet canonicalized)
    int versionMajor = 1;
    int versionMinor = 1;
    Headers headers;
    std::string body;            // fully read, within the transport's limits
    std::string peerAddress;     // numeric IP of the connection's other end
};

struct HttpResponse {
    int status = 200;
    Headers headers;
    std::string body;

    // A plain-text response, e.g. for errors.
    static HttpResponse text(int status, std::string body);
};

// "OK", "Not Found", …; "Unknown" for codes we don't list.
const char* statusReason(int status);

}  // namespace fourfold
