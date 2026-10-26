// CGI: one request per process (overview §8, Hosting modes). This is M5's
// spike (risk R8): enough to measure cold starts and to check that sessions
// and data files stay exact across processes. The full adapter, with a
// cached compiled app, FastCGI and `fourfold cgi-setup`, is M10's.
#pragma once

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace fourfold::cli {

// The CGI environment: a variable's value, or nullopt if unset.
using CgiEnvironment = std::function<std::optional<std::string>(const std::string& name)>;

// Serves one request from the CGI environment and `in` (the body) for the
// app at `folder`, writing the CGI response to `out` and problems to `err`.
// If `timing` is set, how long each step took is written to `err`.
int runCgi(const std::string& folder, const CgiEnvironment& env, std::istream& in, std::ostream& out,
           std::ostream& err, bool timing);

}  // namespace fourfold::cli
