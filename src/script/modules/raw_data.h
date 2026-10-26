// RawData: a buffer of bytes, for binary files (M5.1). Fourfold's own,
// modeled on command-line MiniScript's and keeping its API, so scripts work
// alike in both: `new RawData`, then `resize` (or `len = n`), the typed
// accessors (`byte`/`setByte` ... `double`/`setDouble`, `utf8`/`setUtf8`),
// and `littleEndian`.
//
// An instance is a map whose `__isa` is the frozen class map and whose
// `_handle` is a handle (Value::NewHandle) to the bytes. Every handle is
// checked against the live buffers before use, so a script that points
// `_handle` at anything else gets an empty buffer, never a misread one.
//
// Buffers live outside MS2's heap, so they're capped: each at the per-file
// limit, and all of one run's together at a total (ScriptBudget), counted
// until the garbage collector frees them.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "../ms2.h"

namespace fourfold {

struct Run;

// A run's RawData bytes, shared by its buffers: a buffer freed after its run
// has ended still gives its bytes back. Script thread only.
struct RawBudget {
    size_t used = 0;
    size_t maxBuffer = 0;
    size_t maxTotal = 0;
};

// A new instance holding `bytes`, charged to the run's budget; an error value
// if it's over a limit.
MiniScript::Value newRawData(Run& run, std::string bytes);

// The bytes of a RawData instance (empty if it has no buffer yet), or null if
// `v` isn't one.
const std::string* rawDataBytes(MiniScript::Value v);

// Whether `v` is a RawData instance (its `__isa` is the class).
bool isRawData(MiniScript::Value v);

}  // namespace fourfold
