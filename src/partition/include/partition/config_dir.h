// ConfigDir: private/config/, the app's configuration (overview §1, §10),
// read-only. It's written by the developer or admin, never by the engine, so
// even an exploited engine bug can't rewrite its own policy.
//
// Its files (app.ms, secrets.ms) are MiniScript evaluated as data, so, like
// code, they reach the evaluator only as ConfigFile, which only ConfigDir
// makes: nothing in data/ can ever be passed off as config.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/fd.h"
#include "base/result.h"

namespace fourfold {

class ConfigFile {
public:
    // "private/config/app.ms", or the host path of a server config.
    const std::string& name() const { return name_; }
    const std::string& contents() const { return contents_; }

private:
    friend class ConfigDir;
    ConfigFile(std::string name, std::string contents) : name_(std::move(name)), contents_(std::move(contents)) {}

    std::string name_;
    std::string contents_;
};

class ConfigDir {
public:
    // The largest config file read.
    static constexpr size_t kMaxBytes = 1024 * 1024;

    ConfigDir(ConfigDir&&) = default;
    ConfigDir& operator=(ConfigDir&&) = default;

    // The file `name` ("app.ms"; no folders), or nullopt if there's none.
    // A symbolic link or other non-regular file is an error, not missing.
    Result<std::optional<ConfigFile>> read(std::string_view name) const;

    // A file's identity for change detection: "size:mtime", or "" if it's
    // missing.
    std::string stamp(std::string_view name) const;

    // The server config (overview §10, "Two levels"): a file outside every
    // app, owned by the server's admin, named by host path.
    static Result<ConfigFile> readServerConfig(const std::string& hostPath);

private:
    friend class AppFolder;
    explicit ConfigDir(Fd fd) : fd_(std::move(fd)) {}

    Fd fd_;
};

}  // namespace fourfold
