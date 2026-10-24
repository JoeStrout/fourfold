# Benchmarks: built with the tests, but not run by ctest. Run them from a
# release build (cmake --preset release) for meaningful numbers.

add_executable(bench_script bench_script.cpp)
target_link_libraries(bench_script PRIVATE fourfold::script fourfold_test_support)
fourfold_set_options(bench_script)
