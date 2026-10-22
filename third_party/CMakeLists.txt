# Third-party libraries, vendored as source, one folder each, with the
# upstream license file and a README naming the version.
#
# Planned (Arch §9): an HTTP/1.1 parser and event loop (M1), md4c (M4),
# SQLite (M5), crypto (M5/M6), TLS and HTTP/2 (M11).

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
