#include <iostream>

#include "cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return fourfold::cli::run(args, std::cout, std::cerr);
}
