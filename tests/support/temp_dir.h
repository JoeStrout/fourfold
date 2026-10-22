// TempDir: a scratch folder for a test, deleted when it goes out of scope.
// (Tests may touch files directly; only Fourfold's own code is limited to
// the partition types.)
#pragma once

#include <string>

namespace fourfold::test {

class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }
    // path() + "/" + rel
    std::string at(const std::string& rel) const;

    // Writes a file, creating any missing folders above it.
    void write(const std::string& rel, const std::string& contents) const;
    void makeDir(const std::string& rel) const;
    // Makes a symbolic link at `rel` whose contents are `target`.
    void symlink(const std::string& target, const std::string& rel) const;

private:
    std::string path_;
};

}  // namespace fourfold::test
