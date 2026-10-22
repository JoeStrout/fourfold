#include "base/result.h"

namespace fourfold {

const char* errorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::Invalid:  return "Invalid";
        case ErrorCode::Denied:   return "Denied";
        case ErrorCode::Conflict: return "Conflict";
        case ErrorCode::TooLarge: return "TooLarge";
        case ErrorCode::WouldBlock: return "WouldBlock";
        case ErrorCode::IO:       return "IO";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

std::string Error::toString() const {
    std::string s = errorCodeName(code);
    if (!message.empty()) {
        s += ": ";
        s += message;
    }
    return s;
}

}  // namespace fourfold
