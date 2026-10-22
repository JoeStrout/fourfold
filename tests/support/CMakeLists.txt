# Shared test support: doctest and its main(), plus (later) fakes of the
# store and service interfaces, and temporary app folders.

add_library(fourfold_test_support STATIC test_main.cpp temp_dir.cpp)
target_include_directories(fourfold_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fourfold_test_support PUBLIC doctest)
target_compile_definitions(fourfold_test_support PUBLIC
    FOURFOLD_TEST_APPS_DIR="${CMAKE_SOURCE_DIR}/tests/apps"
    FOURFOLD_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests")
