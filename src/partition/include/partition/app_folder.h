// AppFolder: an app's folder and its partitions (overview §1).
//
//   myapp/
//     code/  data/  dropbox/  private/config/  private/state/
//
// Opening it checks the layout once, up front. A partition folder may be a
// symbolic link (e.g. code/ to a checkout elsewhere): each is resolved to its
// real path here, once, and the app is refused if any partition is inside
// another (code/ pointing into data/, say), since that would undo the
// partitions' separation. Inside a partition, links are never followed.
//
// Host paths appear in this class's errors, which go to the developer.
// They must never reach app code (SANDBOXING.md rule 5); the partition
// types' errors use partition-relative paths only.
#pragma once

#include <string>

#include "base/fd.h"
#include "base/result.h"
#include "partition/code_tree.h"
#include "partition/data_dir.h"

namespace fourfold {

class AppFolder {
public:
    enum class Missing {
        Fail,     // a missing partition folder is an error
        Create,   // create missing folders (and the app folder itself): `fourfold dev`
    };

    static Result<AppFolder> open(const std::string& root, Missing missing = Missing::Fail);

    AppFolder(AppFolder&&) = default;
    AppFolder& operator=(AppFolder&&) = default;

    // The app folder's real path.
    const std::string& root() const { return root_; }

    // A read-only view of code/.
    Result<CodeTree> codeTree(CodeTreeLimits limits = {}) const;
    // data/, dropbox/ and private/state/. Each call opens a new handle.
    Result<DataDir> dataDir() const;
    Result<Dropbox> dropbox() const;
    Result<StateDir> stateDir() const;

private:
    struct Folders {
        Fd code, data, dropbox, state;
        std::string stateReal;   // private/state/'s real path, for SQLite
    };
    AppFolder(std::string root, Folders folders) : root_(std::move(root)), folders_(std::move(folders)) {}

    std::string root_;
    Folders folders_;
};

}  // namespace fourfold
