# Fourfold's components, lowest layer first (Arch §2). Each component may
# depend only on components declared above it; the DEPS lists here are the
# whole dependency graph.

if(FOURFOLD_BUILD_TESTS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/support ${CMAKE_BINARY_DIR}/tests/support)
endif()

# --- Foundation ---
fourfold_component(base)
target_compile_definitions(ff_base PRIVATE FOURFOLD_VERSION="${PROJECT_VERSION}")
fourfold_component(partition DEPS base)             # the four file areas; path rules
fourfold_component(platform DEPS base)             # event loop, sockets; later threads, log, sqlite

# --- Libraries ---
fourfold_component(image DEPS base partition)      # AppImage: the compiled app

# --- Subsystems ---
fourfold_component(compiler DEPS base partition image)

# --- Engine ---
fourfold_component(http DIR engine/http)           # HttpRequest/HttpResponse
fourfold_component(pipeline DIR engine/pipeline DEPS base partition image http)

# --- Hosts ---
fourfold_component(httpserver DIR hosts/http-server DEPS base platform http LIBS llhttp)
fourfold_executable(fourfold DIR hosts/cli
    DEPS base partition compiler pipeline platform httpserver)
