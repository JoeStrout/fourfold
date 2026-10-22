#include "cli.h"

#include <ostream>

#include "base/version.h"
#include "compiler/compiler.h"
#include "httpserver/http_server.h"
#include "partition/app_folder.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"

namespace fourfold::cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr uint16_t kDefaultPort = 8080;

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

    auto app = AppFolder::open(dev.folder, AppFolder::Missing::Create);
    if (!app) {
        err << "fourfold dev: " << app.error().message << "\n";
        return kExitError;
    }
    auto code = app.value().codeTree();
    if (!code) {
        err << "fourfold dev: " << code.error().message << "\n";
        return kExitError;
    }

    CompileOutput compiled = compile(code.value());
    for (const Diagnostic& d : compiled.diagnostics) err << d.toString() << "\n";
    if (!compiled.ok()) {
        err << "fourfold dev: the app didn't compile.\n";
        return kExitError;
    }
    // Code is read again per request, through a CodeTree of its own.
    auto engineCode = app.value().codeTree();
    if (!engineCode) {
        err << "fourfold dev: " << engineCode.error().message << "\n";
        return kExitError;
    }
    Engine engine(compiled.image, std::move(engineCode).value(), EngineOptions{localhostNames()});

    auto loop = EventLoop::create();
    if (!loop) {
        err << "fourfold dev: " << loop.error().message << "\n";
        return kExitError;
    }
    HttpServer server(*loop.value(), [&](HttpRequest request, Responder respond) {
        HttpResponse response = engine.handle(request);
        out << request.method << " " << request.target << " " << response.status << std::endl;
        respond(std::move(response));
    });
    // Localhost only: dev mode lacks protections that later milestones add.
    if (Status s = server.listen("127.0.0.1", dev.port); !s) {
        err << "fourfold dev: can't listen on port " << dev.port << ": " << s.error().message << "\n";
        return kExitError;
    }

    out << "Fourfold " << versionString() << ", serving " << app.value().root() << "\n"
        << "  http://localhost:" << server.port() << "/\n"
        << "Dev mode: localhost only. This build has no CSRF protection, sign-in, or\n"
        << "recompiling yet; restart to see changes. Press Ctrl-C to stop.\n"
        << std::flush;
    loop.value()->run();
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
