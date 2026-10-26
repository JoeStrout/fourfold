# Third-party libraries, vendored as source, one folder each, with the
# upstream license file and a README naming the version.
#
# Planned (Arch §9): an HTTP/1.1 parser and event loop (M1), md4c and htmx (M4),
# SQLite and Monocypher (M5), TLS and HTTP/2 (M11).

# doctest: unit tests only. SYSTEM, so its header doesn't trigger warnings.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

# llhttp: HTTP/1.1 parser (generated C). Used by the http-server host.
add_library(llhttp STATIC
    llhttp/src/api.c
    llhttp/src/http.c
    llhttp/src/llhttp.c)
target_include_directories(llhttp SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/include)
set_target_properties(llhttp PROPERTIES C_STANDARD 99)

# md4c: Markdown parser (C). Used by the markdown component. The entity table
# is for decoding entities in link destinations before they're checked.
add_library(md4c STATIC
    md4c/src/md4c.c
    md4c/src/entity.c)
target_include_directories(md4c SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/md4c/src)
set_target_properties(md4c PROPERTIES C_STANDARD 99)

# SQLite: the engine's own stores in private/state/ (sessions, from M5), and
# later the app's SQLite databases. Reached only through platform's wrapper.
add_library(sqlite3 STATIC sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0)
set_target_properties(sqlite3 PROPERTIES C_STANDARD 99)
find_package(Threads REQUIRED)
target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Monocypher: hashing (BLAKE2b) and constant-time comparison, behind base's
# crypto.h.
add_library(monocypher STATIC monocypher/monocypher.c)
target_include_directories(monocypher SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99)
