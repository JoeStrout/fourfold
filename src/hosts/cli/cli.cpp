#include "cli.h"

#include <ostream>

#include <cstdlib>
#include <iostream>

#include "app_server.h"
#include "base/version.h"
#include "cgi.h"

namespace fourfold::cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr uint16_t kDefaultPort = 8090;

void printUsage(std::ostream& out) {
    out << "Usage: fourfold <command> [arguments]\n"
           "\n"
           "Commands:\n"
           "  dev <app-folder> [--port N]   Serve an app on localhost for development\n"
           "  serve <app-folder> [--listen ADDRESS:PORT] [--host NAME]... [--plain-http]\n"
           "                                Serve an app for real use, behind a proxy that\n"
           "                                provides HTTPS\n"
           "  version                       Print the Fourfold version\n"
           "  help                          Show this message\n";
}

struct DevArgs {
    std::string folder;
    uint16_t port = kDefaultPort;
};

// A port number, 1 to 65535.
bool parsePort(const std::string& p, uint16_t& out) {
    bool digits = !p.empty() && p.size() <= 5 && p.find_first_not_of("0123456789") == std::string::npos;
    unsigned long n = digits ? std::stoul(p) : 0;
    if (n == 0 || n > 65535) return false;
    out = static_cast<uint16_t>(n);
    return true;
}

// Fills `out`, or explains the problem in `error`.
bool parseDevArgs(const std::vector<std::string>& args, DevArgs& out, std::string& error) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--port") {
            if (i + 1 >= args.size()) {
                error = "--port needs a number";
                return false;
            }
            const std::string& p = args[++i];
            if (!parsePort(p, out.port)) {
                error = "invalid port '" + p + "'";
                return false;
            }
        } else if (!a.empty() && a[0] == '-') {
            error = "unknown option '" + a + "'";
            return false;
        } else if (out.folder.empty()) {
            out.folder = a;
        } else {
            error = "more than one app folder given";
            return false;
        }
    }
    if (out.folder.empty()) {
        error = "no app folder given";
        return false;
    }
    return true;
}

int runDev(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DevArgs dev;
    std::string problem;
    if (!parseDevArgs(args, dev, problem)) {
        err << "fourfold dev: " << problem << "\nUsage: fourfold dev <app-folder> [--port N]\n";
        return kExitUsage;
    }

    auto server = AppServer::startDev(dev.folder, dev.port, out, err);
    if (!server) return kExitError;

    out << "Fourfold " << versionString() << ", serving " << server->root() << "\n"
        << "  http://localhost:" << server->port() << "/\n"
        << "Dev mode: localhost only, plain HTTP, with error details shown in the browser.\n"
        << "Watching code/: saved changes are recompiled, and open pages reload.\n"
        << "Press Ctrl-C to stop.\n"
        << std::flush;
    server->loop().run();
    return kExitOk;
}

struct ServeArgs {
    std::string folder;
    std::string address = "127.0.0.1";
    uint16_t port = kDefaultPort;
    std::vector<std::string> hosts;
    bool plainHttp = false;
};

bool isLoopback(const std::string& address) {
    return address == "127.0.0.1" || address == "::1" || address == "localhost";
}

bool parseServeArgs(const std::vector<std::string>& args, ServeArgs& out, std::string& error) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--listen" || a == "--host") {
            if (i + 1 >= args.size()) {
                error = a + " needs a value";
                return false;
            }
            const std::string& v = args[++i];
            if (a == "--host") {
                if (v.empty() || v.find_first_of(":/ ") != std::string::npos) {
                    error = "invalid host name '" + v + "' (give the name alone, like example.com)";
                    return false;
                }
                out.hosts.push_back(v);
                continue;
            }
            // ADDRESS:PORT, with an IPv6 address in brackets: [::]:8090.
            size_t colon = v.rfind(':');
            if (colon == std::string::npos || !parsePort(v.substr(colon + 1), out.port)) {
                error = "--listen needs ADDRESS:PORT, like 0.0.0.0:8090";
                return false;
            }
            out.address = v.substr(0, colon);
            if (out.address.size() >= 2 && out.address.front() == '[' && out.address.back() == ']') {
                out.address = out.address.substr(1, out.address.size() - 2);
            }
            if (out.address.empty()) {
                error = "--listen needs an address, like 0.0.0.0:8090";
                return false;
            }
        } else if (a == "--plain-http") {
            out.plainHttp = true;
        } else if (!a.empty() && a[0] == '-') {
            error = "unknown option '" + a + "'";
            return false;
        } else if (out.folder.empty()) {
            out.folder = a;
        } else {
            error = "more than one app folder given";
            return false;
        }
    }
    if (out.folder.empty()) {
        error = "no app folder given";
        return false;
    }
    if (out.hosts.empty() && !isLoopback(out.address)) {
        error = "give the site's host name with --host (requests for any other name are refused)";
        return false;
    }
    return true;
}

int runServe(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    ServeArgs serve;
    std::string problem;
    if (!parseServeArgs(args, serve, problem)) {
        err << "fourfold serve: " << problem
            << "\nUsage: fourfold serve <app-folder> [--listen ADDRESS:PORT] [--host NAME]... [--plain-http]\n";
        return kExitUsage;
    }
    ServerOptions options;
    options.dev = false;
    options.address = serve.address;
    options.port = serve.port;
    options.hosts = serve.hosts;
    options.https = !serve.plainHttp;

    auto server = AppServer::start(serve.folder, options, out, err);
    if (!server) return kExitError;

    out << "Fourfold " << versionString() << ", serving " << server->root() << "\n"
        << "  listening on " << serve.address << " port " << server->port() << "\n"
        << "Engine log: private/state/logs/. To load changed code, touch code/.deploy.\n"
        << "\n"
        << "This build is not yet ready to face the internet on its own:\n"
        << "  - No TLS. Put it behind a proxy that provides HTTPS"
        << (serve.plainHttp ? " (--plain-http: cookies aren't marked Secure, so\n    use plain HTTP only on a network you trust)" : "")
        << ".\n"
        << "  - No per-request memory limit yet: a script can use as much memory as the\n"
        << "    machine has (time and instruction limits do apply).\n"
        << "  - No sign-in, access rules or rate limits yet.\n"
        << "Press Ctrl-C to stop.\n"
        << std::flush;
    server->loop().run();
    return kExitOk;
}

}  // namespace

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        printUsage(err);
        return kExitUsage;
    }
    const std::string& command = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "help" || command == "--help" || command == "-h") {
        printUsage(out);
        return kExitOk;
    }
    if (command == "version" || command == "--version") {
        out << "fourfold " << versionString() << "\n";
        return kExitOk;
    }
    if (command == "dev") {
        return runDev(rest, out, err);
    }
    if (command == "serve") {
        return runServe(rest, out, err);
    }
    if (command == "cgi") {
        // Unlisted: M5's CGI spike. `fourfold cgi <app-folder>`, run by a web
        // server as a CGI program; FOURFOLD_CGI_TIMING=1 reports timings.
        if (rest.size() != 1) {
            err << "Usage: fourfold cgi <app-folder>\n";
            return kExitUsage;
        }
        auto env = [](const std::string& name) -> std::optional<std::string> {
            const char* v = std::getenv(name.c_str());
            return v ? std::optional<std::string>(v) : std::nullopt;
        };
        return runCgi(rest[0], env, std::cin, out, err, env("FOURFOLD_CGI_TIMING").value_or("") == "1");
    }
    err << "fourfold: unknown command '" << command << "'\n\n";
    printUsage(err);
    return kExitUsage;
}

}  // namespace fourfold::cli
