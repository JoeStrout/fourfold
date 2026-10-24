// The bridge between MiniScript values and Fourfold's plain C++ data
// (Arch §4): the one place values are checked for being inert, the request
// map, and the handles that redirect(), json() and status() return.
#pragma once

#include <optional>
#include <string>

#include "base/datum.h"
#include "ms2.h"

namespace fourfold {

// A MiniScript string's bytes (or any value's str() form).
std::string toStdString(MiniScript::Value v);
// A MiniScript string. MiniScript strings can't hold NUL bytes, so each
// becomes U+FFFD.
MiniScript::Value toValue(const std::string& s);

// The inert check (Arch §4): null, finite numbers, strings, and lists and
// maps of them (map keys must be strings; `__isa` is left out). Fails, with
// `error` set, on functions, handles, error values, non-finite numbers, and
// anything nested more than 64 deep (which also catches cycles).
std::optional<Datum> toDatum(MiniScript::Value v, std::string& error);

// What redirect(), json() and status() return: a handle (Value::NewHandle),
// so script code can't forge one (overview §4, Return values).
struct OutcomeHandle {
    enum class Kind { Redirect, Json, Status };

    Kind kind = Kind::Redirect;
    std::string location;      // Redirect
    std::string body;          // Json: the JSON text; Status: the body
    std::string contentType;   // Status: the body's type, if it has a body
    int status = 0;            // Status
};

MiniScript::Value makeOutcomeHandle(OutcomeHandle outcome);
// The handle's contents, or null if `v` isn't one of these handles.
const OutcomeHandle* outcomeHandleOf(MiniScript::Value v);

}  // namespace fourfold
