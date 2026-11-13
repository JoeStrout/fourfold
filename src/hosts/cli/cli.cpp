#include "cli.h"

#include <ostream>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <thread>

#include "app_server.h"
#include "base/version.h"
#include "cgi.h"
#include "data_commands.h"

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
           "  serve <app-folder> [--tls] [--listen ADDRESS:PORT] [--host NAME]...\n"
           "        [--redirect-listen ADDRESS:PORT | --no-redirect] [--plain-http]\n"
           "        [--server-config FILE] [--migrate]\n"
           "                                Serve an app for real use: with --tls, on its\n"
           "                                own HTTPS (certificates from Let's Encrypt, port\n"
           "                                80 redirecting); otherwise behind a proxy that\n"
           "                                provides HTTPS (--migrate: apply pending\n"
           "                                migrations first)\n"
           "  migrate <app-folder>          Apply the app's pending migrations (asking its\n"
           "                                server, if it's running), after a backup\n"
           "  backup <app-folder>           Back up the app's data and state\n"
           "  restore <app-folder> [NAME]   List the app's backups, or restore one (with\n"
           "        [--data-only]           the app stopped)\n"
           "  cgi-setup <app-folder> <web-root> [--fastcgi]\n"
           "                                Serve an app through Apache's CGI (shared\n"
           "                                hosting): writes the web root's entry point\n"
           "  fastcgi <app-folder> [--listen PATH|HOST:PORT]\n"
           "                                Serve an app through FastCGI (what cgi-setup\n"
           "                                --fastcgi's entry point runs)\n"
           "  run-jobs <app-folder>         Run due jobs, for an app served through CGI\n"
           "        [--max-seconds N]       (for cron, every minute)\n"
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

// ADDRESS:PORT, with an IPv6 address in brackets: [::]:8090.
bool parseListen(const std::string& v, std::string& address, uint16_t& port) {
    size_t colon = v.rfind(':');
    if (colon == std::string::npos || !parsePort(v.substr(colon + 1), port)) return false;
    address = v.substr(0, colon);
    if (address.size() >= 2 && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    return !address.empty();
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

// SIGTERM and SIGINT, for a server: the first shuts it down gracefully, a
// second stops it at once. Construct before starting the server, so every
// thread it starts has the signals blocked, and only run()'s waiter takes
// them.
class StopSignals {
public:
    StopSignals() {
        // A client gone mid-write is an error to handle, not a reason to die.
        std::signal(SIGPIPE, SIG_IGN);
        sigemptyset(&set_);
        sigaddset(&set_, SIGTERM);
        sigaddset(&set_, SIGINT);
        pthread_sigmask(SIG_BLOCK, &set_, &previous_);
    }
    ~StopSignals() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

    void run(AppServer& server, std::chrono::milliseconds deadline) {
        std::atomic<bool> finished{false};
        std::thread waiter([&] {
            int count = 0;
            for (;;) {
                int signal = 0;
                if (sigwait(&set_, &signal) != 0 || finished) return;
                if (count++ == 0) {
                    server.loop().post([&server, deadline] { server.shutdown(deadline); });
                } else {
                    server.loop().stop();
                }
            }
        });
        server.loop().run();
        finished = true;
        pthread_kill(waiter.native_handle(), SIGTERM);   // wakes it, to see `finished`
        waiter.join();
    }

private:
    sigset_t set_, previous_;
};

int runDev(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DevArgs dev;
    std::string problem;
    if (!parseDevArgs(args, dev, problem)) {
        err << "fourfold dev: " << problem << "\nUsage: fourfold dev <app-folder> [--port N]\n";
        return kExitUsage;
    }

    StopSignals signals;
    auto server = AppServer::startDev(dev.folder, dev.port, out, err);
    if (!server) return kExitError;

    out << "Fourfold " << versionString() << ", serving " << server->root() << "\n"
        << "  http://localhost:" << server->port() << "/\n"
        << "Dev mode: localhost only, plain HTTP, with error details shown in the browser.\n"
        << "Watching code/: saved changes are recompiled, and open pages reload.\n"
        << "Press Ctrl-C to stop.\n"
        << std::flush;
    signals.run(*server, std::chrono::seconds(2));
    return kExitOk;
}

struct ServeArgs {
    std::string folder;
    std::string address = "127.0.0.1";
    uint16_t port = kDefaultPort;
    bool listenGiven = false;
    bool tls = false;
    std::string redirectAddress = "::";
    uint16_t redirectPort = 80;
    bool redirect = true;
    std::vector<std::string> hosts;
    bool plainHttp = false;
    std::string serverConfig;
    bool migrate = false;
    bool allowOlderCode = false;
};

bool parseServeArgs(const std::vector<std::string>& args, ServeArgs& out, std::string& error) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--server-config") {
            if (i + 1 >= args.size()) {
                error = a + " needs a file";
                return false;
            }
            out.serverConfig = args[++i];
        } else if (a == "--listen" || a == "--host") {
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
            if (!parseListen(v, out.address, out.port)) {
                error = "--listen needs ADDRESS:PORT, like 0.0.0.0:8090";
                return false;
            }
            out.listenGiven = true;
        } else if (a == "--redirect-listen") {
            if (i + 1 >= args.size() || !parseListen(args[i + 1], out.redirectAddress, out.redirectPort)) {
                error = "--redirect-listen needs ADDRESS:PORT, like [::]:80";
                return false;
            }
            i++;
        } else if (a == "--no-redirect") {
            out.redirect = false;
        } else if (a == "--tls") {
            out.tls = true;
        } else if (a == "--plain-http") {
            out.plainHttp = true;
        } else if (a == "--migrate") {
            out.migrate = true;
        } else if (a == "--allow-older-code") {
            out.allowOlderCode = true;
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

int runServe(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    ServeArgs serve;
    std::string problem;
    if (!parseServeArgs(args, serve, problem)) {
        err << "fourfold serve: " << problem
            << "\nUsage: fourfold serve <app-folder> [--tls] [--listen ADDRESS:PORT] [--host NAME]... "
               "[--redirect-listen ADDRESS:PORT | --no-redirect] [--plain-http] [--server-config FILE] [--migrate]\n";
        return kExitUsage;
    }
    if (serve.tls && serve.plainHttp) {
        err << "fourfold serve: --tls and --plain-http don't go together\n";
        return kExitUsage;
    }
    ServerOptions options;
    options.dev = false;
    options.address = serve.address;
    options.port = serve.port;
    if (serve.tls && !serve.listenGiven) {
        options.address = "::";
        options.port = 443;
    }
    options.hosts = serve.hosts;
    options.https = !serve.plainHttp;
    options.tls = serve.tls;
    if (serve.tls && serve.redirect) {
        options.redirectAddress = serve.redirectAddress;
        options.redirectPort = serve.redirectPort;
    }
    options.serverConfig = serve.serverConfig;
    options.migrateOnStart = serve.migrate;
    options.allowOlderCode = serve.allowOlderCode;

    // systemd's socket activation: the sockets it opened (so Fourfold needs
    // no privilege for ports 80 and 443), named by FileDescriptorName=
    // "https" and "http", or else in order.
    std::vector<InheritedSocket> inherited = takeSystemdSockets();
    for (size_t i = 0; i < inherited.size(); i++) {
        const std::string& name = inherited[i].name;
        bool main = name == "https" || (!serve.tls && name == "http") || (name != "http" && name != "https" && i == 0);
        auto fd = std::make_shared<Fd>(std::move(inherited[i].fd));
        if (main && !options.listenSocket) {
            options.listenSocket = fd;
        } else if (serve.tls && !options.redirectSocket) {
            options.redirectSocket = fd;
            if (options.redirectAddress.empty()) options.redirectAddress = "(systemd)";
        }
    }

    StopSignals signals;
    auto server = AppServer::start(serve.folder, options, out, err);
    if (!server) return kExitError;

    std::string where = options.listenSocket ? "a socket from systemd" : options.address + " port " +
                                                                           std::to_string(server->port());
    out << "Fourfold " << versionString() << ", serving " << server->root() << "\n"
        << "  listening on " << where << (serve.tls ? " (HTTPS" + std::string(server->config()->tls.http2 ? ", HTTP/2)" : ")") : "")
        << "\n";
    if (server->redirectPort() || options.redirectSocket) {
        out << "  port " << (options.redirectSocket ? std::string("from systemd") : std::to_string(server->redirectPort()))
            << ": redirecting to HTTPS, and answering Let's Encrypt\n";
    }
    out << "Engine log: private/state/logs/. To load changed code, touch code/.deploy.\n";
    if (serve.tls && server->certificates()) {
        out << "Certificates come from " << (server->config()->tls.acmeDirectory.empty() ? kLetsEncryptDirectory
                                                                                        : server->config()->tls.acmeDirectory)
            << "; using it means agreeing to its subscriber agreement.\n";
    }
    out << "\nKnown limits:\n";
    if (!serve.tls) {
        out << "  - No TLS here. Put it behind a proxy that provides HTTPS, or use --tls"
            << (serve.plainHttp ? " (--plain-http: cookies aren't marked Secure, so\n    use plain HTTP only on a network you trust)" : "")
            << ".\n";
    }
    out << "  - No per-request memory limit yet: a script can use as much memory as the\n"
        << "    machine has (time and instruction limits do apply). Set one for the\n"
        << "    process (MemoryMax= in systemd).\n"
        << (!serve.tls && server->config()->proxy.trust.empty()
                ? "  - No trusted proxy (config.proxy.trust): behind a proxy, every visitor seems\n"
                  "    to come from its address, and rate limits count them all together.\n"
                : "")
        << "Ctrl-C or SIGTERM stops it, after the requests in flight.\n"
        << std::flush;
    signals.run(*server, std::chrono::seconds(30));
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
    if (command == "migrate") return runMigrate(rest, out, err);
    if (command == "backup") return runBackup(rest, out, err);
    if (command == "restore") return runRestore(rest, out, err);
    if (command == "run-jobs") return runJobs(rest, out, err);
    if (command == "cgi-setup") return runCgiSetup(rest, out, err);
    if (command == "fastcgi") return runFastCgi(rest, out, err);
    if (command == "cgi") {
        // `fourfold cgi <app-folder>`, run by a web server as a CGI program
        // (the entry point cgi-setup writes); FOURFOLD_CGI_TIMING=1 reports
        // each request's time to the server's error log.
        if (rest.size() != 1) {
            err << "Usage: fourfold cgi <app-folder>\n";
            return kExitUsage;
        }
        CgiVariables vars = processEnvironment();
        bool timing = vars.count("FOURFOLD_CGI_TIMING") && vars.at("FOURFOLD_CGI_TIMING") == "1";
        return runCgi(rest[0], vars, std::cin, out, err, timing);
    }
    err << "fourfold: unknown command '" << command << "'\n\n";
    printUsage(err);
    return kExitUsage;
}

}  // namespace fourfold::cli
