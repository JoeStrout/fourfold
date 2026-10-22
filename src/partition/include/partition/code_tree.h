// CodeTree: the code partition, read-only (overview §1, Arch §5).
//
// It offers listing and reading, and nothing that writes. Reading yields
// CodeFile, which only CodeTree can make; the compiler's entry points take
// CodeFile, so "code is compiled only from code/" is a type rule.
//
// What the tree contains, as far as Fourfold is concerned:
// - regular files and folders whose names pass RelPath's rules;
// - not dot-files or dot-folders (.DS_Store, .git, the .deploy trigger),
//   which are skipped silently;
// - not symbolic links, nor names RelPath rejects, which are skipped and
//   reported in CodeListing::skipped, so the compiler can warn about them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "base/fd.h"
#include "base/result.h"
#include "partition/paths.h"

namespace fourfold {

struct CodeTreeLimits {
    size_t maxFiles = 20000;
    size_t maxDepth = 32;
    uint64_t maxFileBytes = 64 * 1024 * 1024;
};

struct CodeEntry {
    RelPath path;
    uint64_t size = 0;
    int64_t modifiedNs = 0;   // since the epoch
};

struct CodeListing {
    std::vector<CodeEntry> files;       // sorted by path
    std::vector<std::string> skipped;   // "code/pages/x.html: symbolic link (not followed)"
};

class CodeFile {
public:
    const RelPath& path() const { return path_; }
    const std::string& contents() const { return contents_; }
    int64_t modifiedNs() const { return modifiedNs_; }

private:
    friend class CodeTree;
    CodeFile(RelPath path, std::string contents, int64_t modifiedNs)
        : path_(std::move(path)), contents_(std::move(contents)), modifiedNs_(modifiedNs) {}

    RelPath path_;
    std::string contents_;
    int64_t modifiedNs_;
};

class CodeTree {
public:
    CodeTree(CodeTree&&) = default;
    CodeTree& operator=(CodeTree&&) = default;

    // Every file in the tree. Fails (TooLarge) past the limits.
    Result<CodeListing> list() const;

    Result<CodeEntry> stat(const RelPath& path) const;

    // The whole file. Fails with NotFound if it's missing, a symbolic link,
    // or not a regular file, and TooLarge past maxFileBytes.
    Result<CodeFile> read(const RelPath& path) const;

private:
    friend class AppFolder;
    CodeTree(Fd root, CodeTreeLimits limits) : root_(std::move(root)), limits_(limits) {}

    Status walk(Fd folder, const RelPath& dir, size_t depth, CodeListing& out) const;

    Fd root_;
    CodeTreeLimits limits_;
};

}  // namespace fourfold
