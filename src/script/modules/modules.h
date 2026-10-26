// The intrinsics that make HTML, one file each (Arch §5 script): frag,
// markdown, and unsafeHTML. Each returns an Html handle. Registered once by
// initMiniScriptRuntime.
#pragma once

#include <string>

#include "../ms2.h"

namespace fourfold {

void addFrag();
void addMarkdown();
void addUnsafeHtml();

// An intrinsic's error result.
MiniScript::IntrinsicResult intrinsicError(const std::string& message);

}  // namespace fourfold
