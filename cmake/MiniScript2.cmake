# MiniScript 2, built from source as the static library fourfold::ms2.
#
# MS2 is a sibling repository (FOURFOLD_MS2_DIR, default ../miniscript2). It
# has two source trees:
#   cpp/core   hand-written runtime (.cpp and .c)
#   generated  C++ transpiled from the C# reference implementation (*.g.cpp);
#              globbed, since the set changes when MS2 is re-transpiled.
# We leave out its test and debug programs, the editline REPL, and App.g.cpp
# (MS2's own main()).
#
# Only the `script` component should link this (Arch §2). Until that
# component exists (M2), nothing does, and the library isn't built by default.

if(NOT EXISTS ${FOURFOLD_MS2_DIR}/cpp/miniscript.h)
    message(STATUS "MiniScript 2 not found at ${FOURFOLD_MS2_DIR}; fourfold::ms2 unavailable")
    return()
endif()

file(GLOB MS2_SOURCES CONFIGURE_DEPENDS
    ${FOURFOLD_MS2_DIR}/cpp/core/*.cpp
    ${FOURFOLD_MS2_DIR}/cpp/core/*.c
    ${FOURFOLD_MS2_DIR}/generated/*.g.cpp)
list(FILTER MS2_SOURCES EXCLUDE REGEX "/core/(test|debug)_[^/]*$")
list(FILTER MS2_SOURCES EXCLUDE REGEX "/generated/App\\.g\\.cpp$")

add_library(ff_ms2 STATIC EXCLUDE_FROM_ALL ${MS2_SOURCES})
add_library(fourfold::ms2 ALIAS ff_ms2)

# MS2 relies on GNU extensions (computed goto, POSIX functions).
set_target_properties(ff_ms2 PROPERTIES C_STANDARD 99 C_EXTENSIONS ON CXX_EXTENSIONS ON)

# SYSTEM: MS2's headers shouldn't trigger warnings in Fourfold's code.
target_include_directories(ff_ms2 SYSTEM PUBLIC
    ${FOURFOLD_MS2_DIR}/cpp
    ${FOURFOLD_MS2_DIR}/cpp/core
    ${FOURFOLD_MS2_DIR}/generated)
