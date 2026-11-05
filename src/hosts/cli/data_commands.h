// `fourfold migrate`, `fourfold backup` and `fourfold restore`.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace fourfold::cli {

// Each takes the arguments after the command's name; returns the exit code.
int runMigrate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int runBackup(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int runRestore(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace fourfold::cli
