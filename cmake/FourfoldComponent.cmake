# Helpers for declaring Fourfold components and their tests.
#
# A component lives in src/<dir>/:
#   src/<dir>/include/<name>/*.h   public headers  (#include "<name>/foo.h")
#   src/<dir>/**/*.cpp, *.h        implementation and private headers
# and its tests live in tests/<dir>/*_test.cpp.
#
# Only the include/ folder is exported, and only to components that list this
# one in DEPS. So a component can't even include the headers of a component it
# doesn't declare, and the dependency list in src/CMakeLists.txt is the
# dependency graph of Fourfold (Arch §2, §9).

# Warning and sanitizer flags for Fourfold's own targets (not third-party code).
function(fourfold_set_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(FOURFOLD_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(FOURFOLD_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(FOURFOLD_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

# fourfold_component(<name> [DIR <dir>] [DEPS <component>...] [LIBS <target>...])
#
# Defines the static library fourfold::<name> from src/<dir> (default: <name>).
# DEPS are other Fourfold components (by name); LIBS are other targets, such
# as third-party libraries. Both are public: they're part of this component's
# interface. Also defines the test executable test_<name>, if tests/<dir>
# exists.
function(fourfold_component name)
    cmake_parse_arguments(ARG "" "DIR" "DEPS;LIBS" ${ARGN})
    if(NOT ARG_DIR)
        set(ARG_DIR ${name})
    endif()
    set(src_dir ${CMAKE_SOURCE_DIR}/src/${ARG_DIR})
    set(target ff_${name})

    # Sources may be in subfolders (e.g. script/modules/).
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${src_dir}/*.cpp)
    list(FILTER sources EXCLUDE REGEX "^${src_dir}/include/")
    if(NOT sources)
        message(FATAL_ERROR "Component '${name}' has no .cpp files in ${src_dir}")
    endif()

    add_library(${target} STATIC ${sources})
    add_library(fourfold::${name} ALIAS ${target})
    target_include_directories(${target}
        PUBLIC ${src_dir}/include
        PRIVATE ${src_dir})
    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(${target} PUBLIC fourfold::${dep})
    endforeach()
    if(ARG_LIBS)
        target_link_libraries(${target} PUBLIC ${ARG_LIBS})
    endif()
    fourfold_set_options(${target})

    set(test_dir ${CMAKE_SOURCE_DIR}/tests/${ARG_DIR})
    if(FOURFOLD_BUILD_TESTS AND EXISTS ${test_dir})
        fourfold_add_test(${name} ${test_dir} fourfold::${name})
    endif()
endfunction()

# fourfold_executable(<name> [DIR <dir>] [DEPS <component>...] [LIBS <target>...])
#
# Defines an executable from src/<dir> (default: hosts/<name>), with its tests
# in tests/<dir> as for components. The tests link the executable's sources
# other than main.cpp, so keep main.cpp a thin wrapper.
function(fourfold_executable name)
    cmake_parse_arguments(ARG "" "DIR" "DEPS;LIBS" ${ARGN})
    if(NOT ARG_DIR)
        set(ARG_DIR hosts/${name})
    endif()
    set(src_dir ${CMAKE_SOURCE_DIR}/src/${ARG_DIR})

    file(GLOB sources CONFIGURE_DEPENDS ${src_dir}/*.cpp)
    set(main ${src_dir}/main.cpp)
    list(REMOVE_ITEM sources ${main})
    if(NOT sources)
        message(FATAL_ERROR "Executable '${name}' needs .cpp files besides main.cpp in ${src_dir}")
    endif()

    # Everything but main() goes in a library, so tests can link it.
    set(lib ff_${name}_impl)
    add_library(${lib} STATIC ${sources})
    target_include_directories(${lib} PUBLIC ${src_dir})
    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(${lib} PUBLIC fourfold::${dep})
    endforeach()
    if(ARG_LIBS)
        target_link_libraries(${lib} PUBLIC ${ARG_LIBS})
    endif()
    fourfold_set_options(${lib})

    add_executable(${name} ${main})
    target_link_libraries(${name} PRIVATE ${lib})
    fourfold_set_options(${name})

    set(test_dir ${CMAKE_SOURCE_DIR}/tests/${ARG_DIR})
    if(FOURFOLD_BUILD_TESTS AND EXISTS ${test_dir})
        fourfold_add_test(${name} ${test_dir} ${lib})
    endif()
endfunction()

# Builds tests/<dir>/*.cpp into bin/test_<name>, linked with the component
# under test and the shared test support, and registers it with CTest.
function(fourfold_add_test name test_dir under_test)
    file(GLOB test_sources CONFIGURE_DEPENDS ${test_dir}/*.cpp)
    if(NOT test_sources)
        return()
    endif()
    set(target test_${name})
    add_executable(${target} ${test_sources})
    target_link_libraries(${target} PRIVATE ${under_test} fourfold_test_support)
    fourfold_set_options(${target})
    add_test(NAME ${name} COMMAND ${target})
    set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()
