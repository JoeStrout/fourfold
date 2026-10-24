#include "cli.h"

#include <ostream>

#include "base/version.h"
#include "dev_server.h"

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
           "  version                       Print the Fourfold version\n"
           "  help                          Show this message\n";
}

struct DevArgs {
    std::string folder;
    uint16_t port = kDefaultPort;
};

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
            unsigned long n = 0;
            bool digits = !p.empty() && p.size() <= 5 && p.find_first_not_of("0123456789") == std::string::npos;
            if (digits) n = std::stoul(p);
            if (!digits || n == 0 || n > 65535) {
                error = "invalid port '" + p + "'";
                return false;
            }
            out.port = static_cast<uint16_t>(n);
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

    auto server = DevServer::start(dev.folder, dev.port, out, err);
    if (!server) return kExitError;

    out << "Fourfold " << versionString() << ", serving " << server->root() << "\n"
        << "  http://localhost:" << server->port() << "/\n"
        << "Dev mode: localhost only. This build has no CSRF protection or sign-in yet,\n"
        << "so it never listens on other addresses. No recompiling yet either: restart\n"
        << "to see changes. Press Ctrl-C to stop.\n"
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
    err << "fourfold: unknown command '" << command << "'\n\n";
    printUsage(err);
    return kExitUsage;
}

}  // namespace fourfold::cli
