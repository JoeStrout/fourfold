// The compiler: a snapshot of code/ in, an AppImage and diagnostics out
// (Arch §5). It reads only through CodeTree, so it can only ever compile
// what's in the code partition.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "image/app_image.h"
#include "partition/code_tree.h"

namespace fourfold {

struct CompileOptions {
    uint64_t generation = 1;
};

struct CompileOutput {
    // Null if there were errors. Its own diagnostics hold any warnings.
    std::shared_ptr<const AppImage> image;
    // Everything found, errors and warnings, in the order found.
    std::vector<Diagnostic> diagnostics;

    bool ok() const { return image != nullptr; }
};

CompileOutput compile(const CodeTree& code, const CompileOptions& options = {});

}  // namespace fourfold
