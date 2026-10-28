#include "partition/app_folder.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fourfold {

namespace {

struct PartitionSpec {
    const char* name;
    mode_t mode;          // when created
    bool isPartition;     // "private" is only a container for config and state
};

// In creation order (parents first).
const PartitionSpec kPartitions[] = {
    {"code", 0755, true},
    {"data", 0700, true},
    {"dropbox", 0700, true},
    {"private", 0700, false},
    {"private/config", 0700, true},
    {"private/state", 0700, true},
};

Error osError(const std::string& what) {
    return makeError(errno == EACCES || errno == EPERM ? ErrorCode::Denied : ErrorCode::IO,
                     what + ": " + std::strerror(errno));
}

Result<std::string> realPath(const std::string& path) {
    char buffer[PATH_MAX];
    if (!::realpath(path.c_str(), buffer)) {
        if (errno == ENOENT || errno == ENOTDIR) return makeError(ErrorCode::NotFound, path + ": not found");
        return osError(path);
    }
    return std::string(buffer);
}

// True if `inner` is `outer` or below it (both real paths).
bool within(const std::string& inner, const std::string& outer) {
    if (inner == outer) return true;
    if (outer == "/") return true;
    return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 &&
           inner[outer.size()] == '/';
}

// What a new private/config/ starts with (overview §10, "Defaults for
// everything": a hobby app needs no config at all).
const char* const kAppTemplate = R"(// The app's configuration: MiniScript, evaluated as data (no I/O, no
// imports). Every setting has a default, so all of this is optional.
// Changes take effect within a second or so; a config with an error is
// reported, and the previous one stays in use.
config = {}

// The site's domain: required for `fourfold serve`, for sign-in links.
// config.domain = "example.com"

// Sign-in. "admins" get the admin role when they sign in. "signUp": "open"
// lets anyone sign in with their email address; "closed" only existing users.
// config.auth = {"admins": ["you@example.com"], "signUp": "open"}

// Email, for sign-in links. In dev mode, messages are printed instead.
// config.mail = {"transport": "smtp", "from": "noreply@example.com",
//     "smtpHost": "smtp.example.com", "smtpUser": "noreply@example.com",
//     "passwordSecret": "smtpPassword"}
)";

const char* const kSecretsTemplate = R"(// Secrets: passwords and API keys. Keep this file out of version control.
//
// A secret with no "hosts" is engine-only: used for things like the SMTP
// password, and never visible to app code. A secret with "hosts" will be
// usable by app code, but only in requests to those hosts.
secrets = {}

// secrets.smtpPassword = {"value": "..."}
)";

void writeTemplate(int folder, const char* name, const char* contents) {
    Fd fd(openat(folder, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fd.valid()) return;   // a template is a convenience; never fail for one
    std::string text(contents);
    (void)!::write(fd.get(), text.data(), text.size());
}

}  // namespace

Result<AppFolder> AppFolder::open(const std::string& rootArg, Missing missing) {
    if (missing == Missing::Create && mkdir(rootArg.c_str(), 0755) < 0 && errno != EEXIST) {
        return osError(rootArg);
    }
    auto root = realPath(rootArg);
    if (!root) return root.error();
    Fd rootFd(::open(root.value().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!rootFd.valid()) return osError(root.value());

    std::vector<std::pair<const char*, std::string>> reals;
    bool newConfig = false;
    for (const PartitionSpec& spec : kPartitions) {
        struct stat info;
        if (fstatat(rootFd.get(), spec.name, &info, 0) < 0) {
            if (errno != ENOENT) return osError(root.value() + "/" + spec.name);
            if (missing == Missing::Fail) {
                return makeError(ErrorCode::NotFound, root.value() + ": missing " + spec.name +
                                                          "/ folder (`fourfold dev` creates it)");
            }
            if (mkdirat(rootFd.get(), spec.name, spec.mode) < 0) {
                return osError(root.value() + "/" + spec.name);
            }
            newConfig = newConfig || std::string(spec.name) == "private/config";
        } else if (!S_ISDIR(info.st_mode)) {
            return makeError(ErrorCode::Invalid, root.value() + "/" + spec.name + " is not a folder");
        }
        if (!spec.isPartition) continue;
        auto real = realPath(root.value() + "/" + spec.name);
        if (!real) return real.error();
        reals.emplace_back(spec.name, std::move(real).value());
    }

    for (size_t i = 0; i < reals.size(); i++) {
        for (size_t j = 0; j < reals.size(); j++) {
            if (i != j && within(reals[i].second, reals[j].second)) {
                return makeError(ErrorCode::Invalid, std::string(reals[i].first) + "/ (" + reals[i].second +
                                                         ") is inside " + reals[j].first + "/ (" +
                                                         reals[j].second + "); partitions must be separate");
            }
        }
    }

    // reals is in kPartitions order: code, data, dropbox, private/config, private/state.
    Folders folders;
    Fd* fds[] = {&folders.code, &folders.data, &folders.dropbox, &folders.config, &folders.state};
    for (size_t i = 0; i < reals.size(); i++) {
        if (!fds[i]) continue;
        *fds[i] = Fd(::open(reals[i].second.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!fds[i]->valid()) return osError(reals[i].second);
    }
    folders.stateReal = reals[4].second;
    if (newConfig) {
        writeTemplate(folders.config.get(), "app.ms", kAppTemplate);
        writeTemplate(folders.config.get(), "secrets.ms", kSecretsTemplate);
    }
    return AppFolder(std::move(root).value(), std::move(folders));
}

namespace {

Result<Fd> duplicate(const Fd& fd, const char* what) {
    Fd copy(fcntl(fd.get(), F_DUPFD_CLOEXEC, 0));
    if (!copy.valid()) return osError(what);
    return copy;
}

}  // namespace

Result<CodeTree> AppFolder::codeTree(CodeTreeLimits limits) const {
    auto fd = duplicate(folders_.code, "code");
    if (!fd) return fd.error();
    return CodeTree(std::move(fd).value(), limits);
}

Result<ConfigDir> AppFolder::configDir() const {
    auto fd = duplicate(folders_.config, "private/config");
    if (!fd) return fd.error();
    return ConfigDir(std::move(fd).value());
}

Result<DataDir> AppFolder::dataDir() const {
    auto fd = duplicate(folders_.data, "data");
    if (!fd) return fd.error();
    return DataDir(std::move(fd).value());
}

Result<Dropbox> AppFolder::dropbox() const {
    auto fd = duplicate(folders_.dropbox, "dropbox");
    if (!fd) return fd.error();
    return Dropbox(std::move(fd).value());
}

Result<StateDir> AppFolder::stateDir() const {
    auto fd = duplicate(folders_.state, "private/state");
    if (!fd) return fd.error();
    return StateDir(std::move(fd).value(), folders_.stateReal);
}

}  // namespace fourfold
