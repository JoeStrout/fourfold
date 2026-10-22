#include "temp_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>   // mkdtemp, on macOS

namespace fs = std::filesystem;

namespace fourfold::test {

TempDir::TempDir() {
    std::string pattern = (fs::temp_directory_path() / "fourfold-test-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    // Resolve symlinks (macOS's /tmp is one), so tests can compare real paths.
    path_ = fs::canonical(pattern).string();
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
}

std::string TempDir::at(const std::string& rel) const { return path_ + "/" + rel; }

void TempDir::write(const std::string& rel, const std::string& contents) const {
    fs::path p = at(rel);
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << contents;
    if (!out) throw std::runtime_error("can't write " + p.string());
}

void TempDir::makeDir(const std::string& rel) const { fs::create_directories(at(rel)); }

void TempDir::symlink(const std::string& target, const std::string& rel) const {
    fs::path p = at(rel);
    fs::create_directories(p.parent_path());
    fs::create_symlink(target, p);
}

}  // namespace fourfold::test
