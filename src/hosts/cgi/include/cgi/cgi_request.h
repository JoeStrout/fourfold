// Requests and responses in CGI's terms (RFC 3875), for CGI and FastCGI
// alike: a request arrives as environment variables (FastCGI's params are
// the same) and a body; a response leaves as a "Status:" line, headers, and
// the body.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <ostream>
#include <string>

#include "http/message.h"

namespace fourfold {

// The request's variables: REQUEST_METHOD, REQUEST_URI, HTTP_HOST, ...
using CgiVariables = std::map<std::string, std::string>;

// The most a request body may be: a form's, or (multipart) one with
// uploads. Larger bodies are refused with a 413 before any of it is read.
struct CgiLimits {
    size_t maxBodyBytes = 1024 * 1024;
    size_t maxUploadBodyBytes = 33 * 1024 * 1024;
};

// How long the body is (CONTENT_LENGTH), or nullopt if it's over the limits
// (`tooLarge` true) or malformed. A missing length is 0.
std::optional<size_t> cgiBodyLength(const CgiVariables& vars, const CgiLimits& limits, bool* tooLarge);

// The request the variables and `body` describe. Every HTTP_* variable is a
// header (HTTP_USER_AGENT → User-Agent), as are CONTENT_TYPE and
// CONTENT_LENGTH.
HttpRequest cgiRequest(const CgiVariables& vars, std::string body);

// Whether the request came over HTTPS (HTTPS=on, as Apache sets it).
bool cgiHttps(const CgiVariables& vars);

// The response in CGI's form. A streamed body can't be sent: such a
// response must be replaced first.
std::string cgiResponse(const HttpResponse& response);

}  // namespace fourfold
