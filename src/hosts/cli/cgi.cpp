#include "cgi.h"

#include <chrono>
#include <arpa/inet.h>
#include <climits>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <thread>

#include "app_server.h"
#include "cgi/cgi_app.h"
#include "cgi/fastcgi.h"
#include "jobs/job_runner.h"

extern char** environ;

namespace fourfold::cli {

namespace {

std::string realPath(const std::string& path) {
    char buffer[PATH_MAX];
    if (!::realpath(path.c_str(), buffer)) return {};
    return buffer;
}

bool within(const std::string& inner, const std::string& outer) {
    return inner == outer || (inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 &&
                              (outer == "/" || inner[outer.size()] == '/'));
}

int fail(std::ostream& out, std::ostream& err, const std::string& message, int status = 500) {
    err << "fourfold cgi: " << message << "\n";
    out << cgiResponse(HttpResponse::text(status, status == 500 ? "Internal Server Error\n" : statusReason(status)));
    out.flush();
    return 1;
}

}  // namespace

CgiVariables processEnvironment() {
    CgiVariables vars;
    for (char** e = environ; e && *e; e++) {
        std::string entry = *e;
        size_t eq = entry.find('=');
        if (eq != std::string::npos) vars[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return vars;
}

std::string webRootProblem(const std::string& appRoot, const std::string& webRoot) {
    std::string web = realPath(webRoot);
    if (web.empty()) return {};   // nothing there to serve
    if (web == appRoot) return "the web root is the app folder itself; it must be outside it";
    if (within(appRoot, web)) {
        return "the app folder (" + appRoot + ") is inside the web root (" + web +
               "), so the web server could serve its code and data; move it outside (see fourfold cgi-setup)";
    }
    for (const char* partition : {"code", "data", "dropbox", "private"}) {
        if (within(web, appRoot + "/" + partition)) {
            return std::string("the web root is inside the app's ") + partition + "/ folder; it must be outside it";
        }
    }
    return {};
}

int runCgi(const std::string& folder, const CgiVariables& vars, std::istream& in, std::ostream& out,
           std::ostream& err, bool timing) {
    auto start = std::chrono::steady_clock::now();
    std::string root = realPath(folder);
    if (root.empty()) return fail(out, err, "can't find the app folder " + folder);
    // Never serve an app the web server itself could serve (overview §8).
    if (auto docRoot = vars.find("DOCUMENT_ROOT"); docRoot != vars.end() && !docRoot->second.empty()) {
        std::string problem = webRootProblem(root, docRoot->second);
        if (!problem.empty()) return fail(out, err, problem);
    }

    bool tooLarge = false;
    auto length = cgiBodyLength(vars, CgiLimits{}, &tooLarge);
    if (!length) {
        return fail(out, err, tooLarge ? "request body too large" : "bad CONTENT_LENGTH", tooLarge ? 413 : 400);
    }
    std::string body(*length, '\0');
    if (*length > 0) {
        in.read(body.data(), static_cast<std::streamsize>(*length));
        body.resize(static_cast<size_t>(in.gcount()));
    }

    auto app = CgiApp::open(root);
    if (!app) return fail(out, err, app.error().message);
    HttpResponse response = app.value()->handle(cgiRequest(vars, std::move(body)), cgiHttps(vars));
    out << cgiResponse(response);
    out.flush();
    if (timing) {
        err << "fourfold cgi: " << response.status << " in "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms\n";
    }
    return 0;
}

int runJobs(const std::vector<std::string>& args, std::ostream&, std::ostream& err) {
    std::string folder, serverConfig;
    double maxSeconds = 50;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if ((a == "--max-seconds" || a == "--server-config") && i + 1 < args.size()) {
            const std::string& v = args[++i];
            if (a == "--server-config") {
                serverConfig = v;
                continue;
            }
            char* end = nullptr;
            maxSeconds = std::strtod(v.c_str(), &end);
            if (v.empty() || *end || maxSeconds <= 0) {
                err << "fourfold run-jobs: --max-seconds needs a positive number\n";
                return 2;
            }
        } else if (!a.empty() && a[0] != '-' && folder.empty()) {
            folder = a;
        } else {
            err << "Usage: fourfold run-jobs <app-folder> [--max-seconds N] [--server-config FILE]\n";
            return 2;
        }
    }
    if (folder.empty()) {
        err << "Usage: fourfold run-jobs <app-folder> [--max-seconds N] [--server-config FILE]\n";
        return 2;
    }
    // A running server runs the app's jobs itself.
    if (serverRunning(folder)) return 0;

    CgiOptions options;
    options.serverConfig = serverConfig;
    auto app = CgiApp::open(folder, options);
    if (!app) {
        err << "fourfold run-jobs: " << app.error().message << "\n";
        return 1;
    }
    CgiApp& cgi = *app.value();
    // One run at a time: cron may start the next before this one is done.
    auto state = AppFolder::open(folder).value().stateDir();
    if (!state) {
        err << "fourfold run-jobs: " << state.error().message << "\n";
        return 1;
    }
    auto lockFile = state.value().lockFile("run-jobs");
    if (!lockFile) {
        err << "fourfold run-jobs: " << lockFile.error().message << "\n";
        return 1;
    }
    auto runLock = FileLock::tryAcquire(std::move(lockFile).value());
    if (!runLock) {
        err << "fourfold run-jobs: " << runLock.error().message << "\n";
        return 1;
    }
    if (!runLock.value()) return 0;

    auto load = cgi.loadFull();
    if (!load) {
        err << "fourfold run-jobs: " << load.error().message << "\n";
        return 1;
    }
    std::unique_ptr<Engine> engine = cgi.engineFor(load.value(), false);
    FileSchemaGate gate(cgi.schemaLock(), cgi.config()->deploy.migrationWait);
    auto runner = JobRunner::create(cgi.jobQueue(), *engine, cgi.scripts(), &gate, &cgi.log(), systemClock());
    if (!runner) {
        err << "fourfold run-jobs: " << runner.error().message << "\n";
        return 1;
    }
    runner.value()->start();

    // Until nothing is running or due, or the time is up.
    auto start = std::chrono::steady_clock::now();
    auto limit = std::chrono::milliseconds(static_cast<int64_t>(maxSeconds * 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the first look
    while (std::chrono::steady_clock::now() - start < limit) {
        if (runner.value()->running() == 0) {
            auto due = cgi.jobQueue().nextDue();
            if (!due || !due.value() || *due.value() > systemClock().nowMillis()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (runner.value()->running() > 0) {
        cgi.log().write("jobs", "run-jobs stopped after " + std::to_string(maxSeconds) +
                                    " s with jobs still running; they'll run again next time");
    }
    runner.value().reset();
    return 0;
}

namespace {

// A listening socket for --listen: a Unix socket at a path, or HOST:PORT.
Result<int> listenOn(const std::string& spec) {
    if (spec.find('/') != std::string::npos) {
        sockaddr_un addr{};
        if (spec.size() >= sizeof addr.sun_path) return makeError(ErrorCode::Invalid, "socket path too long: " + spec);
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, spec.c_str(), sizeof addr.sun_path - 1);
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return makeError(ErrorCode::IO, std::string("socket: ") + std::strerror(errno));
        ::unlink(spec.c_str());
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 64) < 0) {
            std::string e = std::strerror(errno);
            ::close(fd);
            return makeError(ErrorCode::IO, "can't listen on " + spec + ": " + e);
        }
        ::chmod(spec.c_str(), 0660);
        return fd;
    }
    size_t colon = spec.rfind(':');
    if (colon == std::string::npos) return makeError(ErrorCode::Invalid, "--listen needs a socket path or HOST:PORT");
    std::string host = spec.substr(0, colon);
    int port = std::atoi(spec.c_str() + colon + 1);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (port <= 0 || port > 65535 || ::inet_pton(AF_INET, host.empty() ? "127.0.0.1" : host.c_str(), &addr.sin_addr) != 1) {
        return makeError(ErrorCode::Invalid, "--listen: bad address " + spec);
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return makeError(ErrorCode::IO, std::string("socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 64) < 0) {
        std::string e = std::strerror(errno);
        ::close(fd);
        return makeError(ErrorCode::IO, "can't listen on " + spec + ": " + e);
    }
    return fd;
}

bool isListeningSocket(int fd) {
    int listening = 0;
    socklen_t size = sizeof listening;
    return ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) == 0 && listening;
}

}  // namespace

int runFastCgi(const std::vector<std::string>& args, std::ostream&, std::ostream& err) {
    const char* usage = "Usage: fourfold fastcgi <app-folder> [--listen PATH|HOST:PORT] [--server-config FILE]\n";
    std::string folder, listen, serverConfig;
    for (size_t i = 0; i < args.size(); i++) {
        if ((args[i] == "--listen" || args[i] == "--server-config") && i + 1 < args.size()) {
            (args[i] == "--listen" ? listen : serverConfig) = args[i + 1];
            i++;
        } else if (!args[i].empty() && args[i][0] != '-' && folder.empty()) {
            folder = args[i];
        } else {
            err << usage;
            return 2;
        }
    }
    if (folder.empty()) {
        err << usage;
        return 2;
    }
    std::string root = realPath(folder);
    if (root.empty()) {
        err << "fourfold fastcgi: can't find the app folder " << folder << "\n";
        return 1;
    }

    // SIGTERM and SIGINT are noticed between connections, by this thread:
    // blocked here, before any other thread starts, so they stay pending.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGTERM);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    int listenFd = 0;
    if (!listen.empty()) {
        auto fd = listenOn(listen);
        if (!fd) {
            err << "fourfold fastcgi: " << fd.error().message << "\n";
            return 1;
        }
        listenFd = fd.value();
    } else if (!isListeningSocket(0)) {
        err << "fourfold fastcgi: run by a web server's FastCGI module (stdin must be its listening socket), "
               "or give --listen PATH|HOST:PORT\n";
        return 2;
    }

    CgiOptions options;
    options.serverConfig = serverConfig;
    options.keepVersion = true;
    auto opened = CgiApp::open(root, options);
    if (!opened) {
        err << "fourfold fastcgi: " << opened.error().message << "\n";
        return 1;
    }
    CgiApp& app = *opened.value();
    fastcgi::Handler handler = [&](const CgiVariables& vars, std::string body) {
        if (auto docRoot = vars.find("DOCUMENT_ROOT"); docRoot != vars.end() && !docRoot->second.empty()) {
            std::string problem = webRootProblem(root, docRoot->second);
            if (!problem.empty()) {
                app.log().write("error", problem);
                return HttpResponse::text(500, "Internal Server Error\n");
            }
        }
        return app.handle(cgiRequest(vars, std::move(body)), cgiHttps(vars));
    };

    while (true) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGTERM) || sigismember(&pending, SIGINT) || sigismember(&pending, SIGHUP)) break;
        pollfd p{listenFd, POLLIN, 0};
        int ready = ::poll(&p, 1, 250);
        if (ready <= 0) continue;
        int conn = ::accept(listenFd, nullptr, nullptr);
        if (conn < 0) continue;
        // A web server that stops sending mid-request doesn't hold us forever.
        timeval timeout{120, 0};
        ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        Status s = fastcgi::serveConnection(conn, handler);
        if (!s) app.log().write("error", "FastCGI: " + s.error().message);
        ::close(conn);
    }
    if (!listen.empty()) {
        ::close(listenFd);
        if (listen.find('/') != std::string::npos) ::unlink(listen.c_str());
    }
    return 0;
}

}  // namespace fourfold::cli
