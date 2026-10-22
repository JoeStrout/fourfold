cmake_minimum_required(VERSION 3.21)   # 3.21: CMakePresets.json version 3
project(fourfold VERSION 0.1.0 LANGUAGES C CXX)

# Fourfold's own code is C++17. (MiniScript 2 builds with its own settings;
# see cmake/MiniScript2.cmake.)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd and other tools.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build products: build/<preset>/bin for executables (fourfold, tests),
# build/<preset>/lib for the per-component static libraries.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Some sources are for one OS only (e.g. platform/poller_epoll.cpp) and
# compile to nothing elsewhere; don't let Apple's ranlib warn about that.
if(APPLE)
    set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
endif()

option(FOURFOLD_BUILD_TESTS "Build the unit tests" ON)
option(FOURFOLD_WERROR "Treat compiler warnings as errors" OFF)
option(FOURFOLD_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

set(FOURFOLD_MS2_DIR "${CMAKE_SOURCE_DIR}/../miniscript2"
    CACHE PATH "Path to the MiniScript 2 repository")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(FourfoldComponent)
include(MiniScript2)

if(FOURFOLD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)
