# Fuzzing the template tokenizer and the Markdown renderer (M4).
#
# test_fuzz is an ordinary test (run by ctest): seeded random mutations of
# a corpus, checked for crashes (best under the asan preset), for values
# escaping their context, and for super-linear time on adversarial inputs.
#
# Where the compiler supports libFuzzer (-fsanitize=fuzzer; Homebrew's LLVM
# does, Apple's clang doesn't), fuzz_template and fuzz_markdown are built
# too, for open-ended runs:  ./build/<preset>/bin/fuzz_markdown -max_total_time=300

add_executable(test_fuzz fuzz_test.cpp)
target_link_libraries(test_fuzz PRIVATE fourfold::markdown fourfold_test_support)
fourfold_set_options(test_fuzz)
add_test(NAME fuzz COMMAND test_fuzz)
set_tests_properties(fuzz PROPERTIES LABELS unit)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
check_cxx_source_compiles(
    "#include <cstddef>\nextern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, size_t) { return 0; }"
    FOURFOLD_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(FOURFOLD_HAVE_LIBFUZZER)
    foreach(target fuzz_template fuzz_markdown)
        add_executable(${target} ${target}.cpp)
        target_link_libraries(${target} PRIVATE fourfold::template fourfold::markdown fourfold_test_support)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
