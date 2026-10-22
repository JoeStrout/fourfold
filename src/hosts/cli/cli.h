// The `fourfold` command line: parses arguments and runs a subcommand.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace fourfold::cli {

// Runs the command line `args` (not including the program name), writing
// normal output to `out` and errors to `err`. Returns the process exit code.
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace fourfold::cli
