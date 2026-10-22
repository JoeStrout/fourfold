// Result<T> and Error: how Fourfold reports expected failures.
//
// MiniScript 2 doesn't throw, and neither does Fourfold for failures that are
// part of normal operation (a missing file, a bad path, a malformed request).
// Those come back as an Error inside a Result. Exceptions are reserved for
// bugs and resource exhaustion.
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace fourfold {

enum class ErrorCode {
    NotFound,      // the thing asked for doesn't exist
    Invalid,       // the input is malformed or breaks a rule
    Denied,        // the operation isn't permitted
    Conflict,      // the input is well formed but clashes with existing state
    TooLarge,      // a size or count limit was exceeded
    WouldBlock,    // a non-blocking operation can't proceed yet; try again later
    IO,            // the OS reported a failure
    Internal,      // a bug in Fourfold
};

const char* errorCodeName(ErrorCode code);

struct Error {
    ErrorCode code;
    std::string message;

    // "NotFound: no such file 'x'"
    std::string toString() const;
};

inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : state_(std::move(value)) {}
    Result(Error error) : state_(std::move(error)) {}

    bool ok() const { return state_.index() == 0; }
    explicit operator bool() const { return ok(); }

    // Precondition: ok().
    T& value() & { return std::get<0>(state_); }
    const T& value() const& { return std::get<0>(state_); }
    T&& value() && { return std::get<0>(std::move(state_)); }

    // Precondition: !ok().
    const Error& error() const { return std::get<1>(state_); }

private:
    std::variant<T, Error> state_;
};

// A Result with no value: success, or an Error.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // Precondition: !ok().
    const Error& error() const { return error_; }

private:
    Error error_{ErrorCode::Internal, {}};
    bool ok_ = true;
};

using Status = Result<void>;

}  // namespace fourfold
