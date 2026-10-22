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

    Fd code(::open(reals[0].second.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!code.valid()) return osError(reals[0].second);
    return AppFolder(std::move(root).value(), std::move(code));
}

Result<CodeTree> AppFolder::codeTree(CodeTreeLimits limits) const {
    Fd fd(fcntl(code_.get(), F_DUPFD_CLOEXEC, 0));
    if (!fd.valid()) return osError("code");
    return CodeTree(std::move(fd), limits);
}

}  // namespace fourfold
