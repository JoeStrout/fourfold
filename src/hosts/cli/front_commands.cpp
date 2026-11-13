#include "front_commands.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ostream>
#include <sys/stat.h>

#include "base/version.h"
#include "config/app_config.h"
#include "dropboxwriter/dropbox_writer.h"
#include "front/front_server.h"
#include "listen_args.h"
#include "partition/config_dir.h"
#include "platform/socket.h"
#include "script/script_host.h"
#include "stop_signals.h"

namespace fourfold::cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

const char* const kFrontUsage =
    "Usage: fourfold front --server-config FILE [--listen ADDRESS:PORT]\n"
    "       [--redirect-listen ADDRESS:PORT | --no-redirect] [--state FOLDER]\n";
const char* const kWriterUsage = "Usage: fourfold dropbox-writer <dropbox-folder> [--listen PATH]\n";

// The server config, read as front reads it: through a script host of its
// own, which is gone again before front starts serving.
std::optional<FrontConfig> readFrontConfig(const std::string& path, std::ostream& err) {
    auto file = ConfigDir::readServerConfig(path);
    if (!file) {
        err << "fourfold front: " << file.error().message << "\n";
        return std::nullopt;
    }
    auto host = ScriptHost::create();
    if (!host) {
        err << "fourfold front: " << host.error().message << "\n";
        return std::nullopt;
    }
    FrontConfigLoad load = loadFrontConfig(file.value(), *host.value());
    for (const Diagnostic& d : load.diagnostics) err << d.toString() << "\n";
    if (!load.config) err << "fourfold front: the server config has errors (above)\n";
    return load.config;
}

}  // namespace

int runFront(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    FrontOptions options;
    std::string serverConfig;
    bool redirect = true;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        bool hasValue = i + 1 < args.size() && !args[i + 1].empty();
        if (a == "--server-config" && hasValue) {
            serverConfig = args[++i];
        } else if (a == "--state" && hasValue) {
            options.stateFolder = args[++i];
        } else if (a == "--listen" && hasValue) {
            if (!parseListen(args[++i], options.address, options.port)) {
                err << "fourfold front: --listen needs ADDRESS:PORT, like [::]:443\n" << kFrontUsage;
                return kExitUsage;
            }
        } else if (a == "--redirect-listen" && hasValue) {
            if (!parseListen(args[++i], options.redirectAddress, options.redirectPort)) {
                err << "fourfold front: --redirect-listen needs ADDRESS:PORT, like [::]:80\n" << kFrontUsage;
                return kExitUsage;
            }
        } else if (a == "--no-redirect") {
            redirect = false;
        } else {
            bool takesValue = a == "--server-config" || a == "--state" || a == "--listen" || a == "--redirect-listen";
            err << "fourfold front: " << (takesValue ? a + " needs a value" : "unknown argument '" + a + "'") << "\n"
                << kFrontUsage;
            return kExitUsage;
        }
    }
    if (serverConfig.empty()) {
        err << "fourfold front: give the server config with --server-config (it lists the apps)\n" << kFrontUsage;
        return kExitUsage;
    }
    if (!redirect) options.redirectAddress.clear();

    // systemd's socket activation: "https" and "http" by name, or in order.
    std::vector<InheritedSocket> inherited = takeSystemdSockets();
    for (size_t i = 0; i < inherited.size(); i++) {
        const std::string& name = inherited[i].name;
        auto fd = std::make_shared<Fd>(std::move(inherited[i].fd));
        if ((name == "https" || (name != "http" && i == 0)) && !options.listenSocket) {
            options.listenSocket = fd;
        } else if (redirect && !options.redirectSocket) {
            options.redirectSocket = fd;
        }
    }

    auto config = readFrontConfig(serverConfig, err);
    if (!config) return kExitError;
    std::vector<FrontApp> apps = config->apps;
    options.config = std::move(*config);
    options.log = [&out](const std::string& line) { out << line << std::endl; };

    StopSignals signals;
    auto loop = EventLoop::create();
    if (!loop) {
        err << "fourfold front: " << loop.error().message << "\n";
        return kExitError;
    }
    auto front = FrontServer::create(*loop.value(), std::move(options));
    if (!front) {
        err << "fourfold front: " << front.error().message << "\n";
        return kExitError;
    }
    FrontServer& f = *front.value();
    out << "Fourfold " << versionString() << ", front for " << f.upstreams().size()
        << (f.upstreams().size() == 1 ? " app" : " apps") << "\n";
    for (const FrontApp& app : apps) {
        std::string names;
        for (const std::string& d : app.domains) names += (names.empty() ? "" : ", ") + d;
        out << "  " << app.name << ": " << names << " -> " << app.socket << "\n";
    }
    out << "  listening on " << (f.port() ? "port " + std::to_string(f.port()) : std::string("a socket from systemd"))
        << " (HTTPS)" << (f.redirectPort() ? ", and port " + std::to_string(f.redirectPort()) + " (redirects)" : "")
        << "\n"
        << "Ctrl-C or SIGTERM stops it, after the requests in flight.\n"
        << std::flush;
    f.start();
    EventLoop& l = *loop.value();
    signals.run(l, [&f, &l] { f.shutdown(std::chrono::seconds(30), [&l] { l.stop(); }); });
    return kExitOk;
}

int runDropboxWriter(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    std::string folder, socketPath;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--listen" && i + 1 < args.size() && !args[i + 1].empty()) {
            socketPath = args[++i];
        } else if (!a.empty() && a[0] != '-' && folder.empty()) {
            folder = a;
        } else {
            err << "fourfold dropbox-writer: unknown argument '" << a << "'\n" << kWriterUsage;
            return kExitUsage;
        }
    }
    if (folder.empty()) {
        err << "fourfold dropbox-writer: give the app's dropbox/ folder\n" << kWriterUsage;
        return kExitUsage;
    }
    std::vector<InheritedSocket> inherited = takeSystemdSockets();
    if (socketPath.empty() && inherited.empty()) {
        err << "fourfold dropbox-writer: give the socket to listen on with --listen (or use systemd's socket "
               "activation)\n"
            << kWriterUsage;
        return kExitUsage;
    }
    Fd dir(::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir.valid()) {
        err << "fourfold dropbox-writer: " << folder << ": " << std::strerror(errno) << "\n";
        return kExitError;
    }

    StopSignals signals;
    auto loop = EventLoop::create();
    if (!loop) {
        err << "fourfold dropbox-writer: " << loop.error().message << "\n";
        return kExitError;
    }
    DropboxWriter writer(*loop.value(), Dropbox(std::move(dir)));
    Status s;
    if (!inherited.empty()) {
        s = writer.adopt(std::move(inherited[0].fd));
    } else {
        auto listening = listenUnix(socketPath, 0660);
        s = listening ? writer.adopt(std::move(listening).value()) : Status(listening.error());
    }
    if (!s) {
        err << "fourfold dropbox-writer: can't listen" << (socketPath.empty() ? "" : " on " + socketPath) << ": "
            << s.error().message << "\n";
        return kExitError;
    }
    out << "Fourfold " << versionString() << ", dropbox writer for " << folder << ", listening on "
        << (socketPath.empty() ? std::string("a socket from systemd") : socketPath) << "\n"
        << std::flush;
    EventLoop& l = *loop.value();
    signals.run(l, [&l] { l.stop(); });
    return kExitOk;
}

}  // namespace fourfold::cli
