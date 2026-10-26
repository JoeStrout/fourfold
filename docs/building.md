# Building Fourfold

## Requirements

- CMake 3.21 or later, and Ninja
- A C++17 compiler: Clang 10+ (including Apple Clang) or GCC 9+
- [MiniScript 2](https://github.com/JoeStrout/miniscript2), checked out next to this repository as `../miniscript2` (or set `FOURFOLD_MS2_DIR`). Configuring fails without it.

On macOS: `xcode-select --install`, then `brew install cmake ninja`. On Debian/Ubuntu: `apt install build-essential cmake ninja-build`.

## Build and test

```sh
cmake --preset dev            # configure (once, or after changing CMake files)
cmake --build --preset dev    # build
ctest --preset dev            # run all tests
./build/dev/bin/fourfold help
```

To run a sample app:

```sh
./build/dev/bin/fourfold dev examples/static-site     # then open http://localhost:8090/
./build/dev/bin/fourfold dev examples/scripts         # pages with MiniScript (M2)
./build/dev/bin/fourfold dev examples/guestbook       # templates, fragments, Markdown and HTMX (M4)
./build/dev/bin/fourfold dev examples/markdown-site   # a site made only of .md files (M4)
```

`fourfold dev` watches `code/`: save a file, and open pages reload (or, for a stylesheet, swap it in place). Compile errors show as an overlay on open pages, while the last version that compiled keeps serving. Touching `code/.deploy` recompiles at once.

Benchmarks are built with the tests but not run by ctest; use a release build for meaningful numbers:

```sh
cmake --preset release && cmake --build --preset release
./build/release/bin/bench_script     # the cost of a fresh VM per request (see docs/design/m2-results.md)
./build/release/bin/bench_compile    # what recompiling costs requests meanwhile (see docs/design/m3-results.md)
```

The fuzz tests (`tests/fuzz/`) run with the others, from a fixed seed: mutated templates and Markdown, checked for crashes and for values escaping their context, and adversarial inputs checked for linear time. Set `FOURFOLD_FUZZ_TIMES=1` to print the timings. Run them under the `asan` preset to catch memory errors. Where the compiler supports libFuzzer (Homebrew's LLVM does; Apple's clang doesn't), `fuzz_template` and `fuzz_markdown` are built too, for open-ended runs:

```sh
./build/dev/bin/fuzz_markdown -max_total_time=300
```

Presets (in `CMakePresets.json`):

| Preset | Build folder | Use |
|---|---|---|
| `dev` | `build/dev` | Everyday debug build |
| `asan` | `build/asan` | Debug, with AddressSanitizer and UBSan |
| `release` | `build/release` | Optimized, with debug info |
| `ci` | `build/ci` | Like `dev`, with warnings as errors |

Run one component's tests directly with `./build/dev/bin/test_<component>`. It takes [doctest's options](https://github.com/doctest/doctest/blob/master/doc/markdown/commandline.md), such as `-tc="Status*"` to run only matching test cases.

For clangd and other editor tooling, link the compile database into the repository root (this link is git-ignored):

```sh
ln -sf build/dev/compile_commands.json compile_commands.json
```

## Repository layout

```
fourfold/
  CMakeLists.txt        top-level build: options, presets' variables, subfolders
  CMakePresets.json     dev / asan / release / ci
  cmake/                build helpers: fourfold_component(), the MiniScript 2 library
  src/
    CMakeLists.txt      every component, lowest layer first, with its DEPS
    <component>/
      include/<component>/*.h   public headers
      *.cpp, *.h                implementation and private headers
    engine/<component>/         engine components (pipeline, policy, …)
    hosts/<program>/            executables; main.cpp is a thin wrapper
  tests/
    support/            doctest's main(), shared fakes and helpers
    <component>/        *_test.cpp for src/<component>, same relative path
    apps/               sample app folders used as test fixtures
    bench/              benchmarks (built, not run by ctest)
  examples/             sample apps that demonstrate each milestone (from M1)
  third_party/          vendored libraries, one folder each, with license files
  docs/                 design notes and this guide
  build/                build products (git-ignored), one folder per preset
    <preset>/bin/       fourfold, test_<component>
    <preset>/lib/       one static library per component
```

## Conventions

**Components.** Each component (Arch §2) is a static library, declared in `src/CMakeLists.txt` with `fourfold_component(name DEPS …)`. Its target is `fourfold::<name>`. Only its `include/` folder is visible, and only to components that list it in `DEPS`; so a component can't include another's headers, or link against it, without declaring the dependency. Components are declared lowest layer first, and may depend only on ones above them in that file. Adding a dependency means editing that list, which makes it a visible decision.

Public headers are included by component name: `#include "base/result.h"`. Private headers are included by bare name from within the component: `#include "codec_impl.h"`.

**Executables.** `fourfold_executable(name DIR hosts/… DEPS …)` builds `main.cpp` into the program, and everything else in the folder into a library the tests link. So keep `main.cpp` to argument passing.

**Tests.** A `.cpp` file in `tests/<dir>/` is added to the test executable for `src/<dir>/` automatically (re-run the build; globs are re-checked). Tests use [doctest](https://github.com/doctest/doctest) (`#include "doctest.h"`; `TEST_CASE`, `SUBCASE`, `CHECK`, `REQUIRE`, …). A component's tests use only its public headers, plus fakes of the interfaces below it. Fixture app folders go in `tests/apps/`; tests find them via the `FOURFOLD_TEST_APPS_DIR` macro.

**Embedded files.** Files the engine serves itself (htmx, the default Markdown stylesheet) are built into the program with `fourfold_embed(component symbol file)`, which turns the file into a byte array at build time (`cmake/Embed.cmake`). So an app can't replace or break them, and there's nothing to install beside the binary.

**Errors.** Expected failures are returned as `Result<T>` or `Status` (`base/result.h`), not thrown.

**Third-party code.** Vendored as source in `third_party/<name>/`, with its license and a README naming the upstream version, and built by `third_party/CMakeLists.txt` without Fourfold's warning flags. Nothing is downloaded at build time, so a fresh checkout builds offline.

**MiniScript 2** is built from source as `fourfold::ms2` (see `cmake/MiniScript2.cmake`). Only the `script` component may link it.
