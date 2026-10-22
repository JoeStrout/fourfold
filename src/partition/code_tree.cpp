#include "partition/code_tree.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "fs_util.h"

namespace fourfold {

namespace {

const std::string kPartition = "code";

// A name as it may safely appear in a message (it may hold control bytes).
std::string printable(const std::string& dir, std::string_view name) {
    std::string s = dir.empty() ? kPartition + "/" : kPartition + "/" + dir + "/";
    for (char ch : name) {
        unsigned char c = static_cast<unsigned char>(ch);
        s += (c < 0x20 || c == 0x7F) ? '?' : ch;
    }
    return s;
}

}  // namespace

Result<CodeListing> CodeTree::list() const {
    auto root = openFolder(root_.get(), RelPath(), kPartition);
    if (!root) return root.error();
    CodeListing listing;
    if (Status s = walk(std::move(root).value(), RelPath(), 0, listing); !s) return s.error();
    std::sort(listing.files.begin(), listing.files.end(),
              [](const CodeEntry& a, const CodeEntry& b) { return a.path < b.path; });
    std::sort(listing.skipped.begin(), listing.skipped.end());
    return listing;
}

Status CodeTree::walk(Fd folder, const RelPath& dir, size_t depth, CodeListing& out) const {
    DIR* d = fdopendir(folder.get());
    if (!d) return fileError(errno, describe(kPartition, dir));
    folder.release();   // the DIR owns it now
    struct Closer {
        DIR* d;
        ~Closer() { closedir(d); }
    } closer{d};

    while (struct dirent* entry = readdir(d)) {
        std::string_view name = entry->d_name;
        if (name.empty() || name[0] == '.') continue;   // ".", "..", and dot-files

        auto child = dir.child(name);
        if (!child) {
            out.skipped.push_back(printable(dir.str(), name) + ": " + child.error().message);
            continue;
        }
        struct stat info;
        if (fstatat(dirfd(d), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;   // removed while we were listing
            return fileError(errno, describe(kPartition, child.value()));
        }
        if (S_ISLNK(info.st_mode)) {
            out.skipped.push_back(describe(kPartition, child.value()) + ": symbolic link (not followed)");
        } else if (S_ISDIR(info.st_mode)) {
            if (depth + 1 >= limits_.maxDepth) {
                return makeError(ErrorCode::TooLarge,
                                 describe(kPartition, child.value()) + ": folders nested too deeply");
            }
            Fd sub(openat(dirfd(d), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!sub.valid()) return fileError(errno, describe(kPartition, child.value()));
            if (Status s = walk(std::move(sub), child.value(), depth + 1, out); !s) return s;
        } else if (S_ISREG(info.st_mode)) {
            if (out.files.size() >= limits_.maxFiles) {
                return makeError(ErrorCode::TooLarge, "code: more than " +
                                                          std::to_string(limits_.maxFiles) + " files");
            }
            out.files.push_back({std::move(child).value(), static_cast<uint64_t>(info.st_size),
                                 modifiedTimeNs(info)});
        } else {
            out.skipped.push_back(describe(kPartition, child.value()) + ": not a regular file");
        }
    }
    return {};
}

Result<CodeEntry> CodeTree::stat(const RelPath& path) const {
    struct stat info;
    auto fd = openFileForRead(root_.get(), path, kPartition, &info);
    if (!fd) return fd.error();
    return CodeEntry{path, static_cast<uint64_t>(info.st_size), modifiedTimeNs(info)};
}

Result<CodeFile> CodeTree::read(const RelPath& path) const {
    struct stat info;
    auto fd = openFileForRead(root_.get(), path, kPartition, &info);
    if (!fd) return fd.error();
    if (static_cast<uint64_t>(info.st_size) > limits_.maxFileBytes) {
        return makeError(ErrorCode::TooLarge, describe(kPartition, path) + ": file too large");
    }
    std::string contents;
    contents.resize(static_cast<size_t>(info.st_size));
    size_t got = 0;
    // The file may change size while we read; stop at the end, or at the limit.
    for (;;) {
        if (got == contents.size()) {
            if (contents.size() >= limits_.maxFileBytes) break;
            contents.resize(std::min<uint64_t>(limits_.maxFileBytes, contents.size() * 2 + 4096));
        }
        ssize_t n = ::read(fd.value().get(), &contents[got], contents.size() - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fileError(errno, describe(kPartition, path));
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    contents.resize(got);
    return CodeFile(path, std::move(contents), modifiedTimeNs(info));
}

}  // namespace fourfold
