// `fourfold front` and `fourfold dropbox-writer`: the two processes besides
// the engines on a server with several apps (overview §8; §1, OS-level
// enforcement).
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace fourfold::cli {

// Each takes the arguments after the command's name; returns the exit code.
int runFront(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int runDropboxWriter(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace fourfold::cli
