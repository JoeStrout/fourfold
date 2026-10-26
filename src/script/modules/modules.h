// Fourfold's intrinsic modules, one file each (Arch §5 script). Registered
// once by initMiniScriptRuntime.
//
// - frag, markdown, unsafeHTML: make HTML; each returns an Html handle.
// - data: the app's data files (data.get, data.set, ...).
// - file: any file in data/, whole (file.readLines, file.saveRaw, ...).
// - RawData: a buffer of bytes, for binary files (raw_data.h).
// - dropbox: the app's append-only output (dropbox.write).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "../ms2.h"

namespace fourfold {

void addFrag();
void addMarkdown();
void addUnsafeHtml();
void addData();
void addFile();
void addRawData();
void addDropbox();

// An intrinsic's error result.
MiniScript::IntrinsicResult intrinsicError(const std::string& message);

// A module: an intrinsic named `name` that returns a frozen map of the given
// functions (as gc does), so scripts write `data.get(...)`. Each function is
// an unnamed intrinsic, made by the caller with Intrinsic::Create("").
void addModule(const char* name, const std::vector<std::pair<const char*, MiniScript::Intrinsic>>& functions);

}  // namespace fourfold
