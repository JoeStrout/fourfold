# Fourfold's components, lowest layer first (Arch §2). Each component may
# depend only on components declared above it; the DEPS lists here are the
# whole dependency graph.

if(FOURFOLD_BUILD_TESTS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/support ${CMAKE_BINARY_DIR}/tests/support)
endif()

# --- Foundation ---
fourfold_component(base)
target_link_libraries(ff_base PRIVATE monocypher utf8proc)
target_compile_definitions(ff_base PRIVATE FOURFOLD_VERSION="${PROJECT_VERSION}")
fourfold_component(partition DEPS base)             # the four file areas; path rules
fourfold_component(platform DEPS base)             # event loop, sockets, workers, random, SQLite
target_link_libraries(ff_platform PRIVATE sqlite3)

# --- Libraries ---
fourfold_component(template DEPS base partition)      # contextual escaping (overview §4)
fourfold_component(markdown DEPS base template LIBS md4c)   # Fourfold Markdown, over md4c's parser
fourfold_component(compiledapp DEPS base partition template)   # CompiledApp: the compiled form of an app

# --- Subsystems ---
fourfold_component(state DEPS base partition platform)   # engine stores in private/state/: sessions, locks, log
fourfold_component(appdata DEPS base partition platform state compiledapp)   # the app's data: files, SQLite, Postgres
# Postgres support, if libpq can be found (pg_config, or FOURFOLD_PG_CONFIG).
find_program(FOURFOLD_PG_CONFIG pg_config)
if(FOURFOLD_PG_CONFIG)
    execute_process(COMMAND ${FOURFOLD_PG_CONFIG} --includedir OUTPUT_VARIABLE pg_include OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${FOURFOLD_PG_CONFIG} --libdir OUTPUT_VARIABLE pg_libdir OUTPUT_STRIP_TRAILING_WHITESPACE)
    find_library(FOURFOLD_LIBPQ NAMES pq HINTS ${pg_libdir} NO_DEFAULT_PATH)
endif()
if(FOURFOLD_LIBPQ)
    message(STATUS "Postgres support: yes (${FOURFOLD_LIBPQ})")
    target_include_directories(ff_appdata SYSTEM PRIVATE ${pg_include})
    target_link_libraries(ff_appdata PRIVATE ${FOURFOLD_LIBPQ})
    target_compile_definitions(ff_appdata PRIVATE FOURFOLD_HAVE_POSTGRES=1)
else()
    message(STATUS "Postgres support: no (libpq not found; install it, or set FOURFOLD_PG_CONFIG)")
endif()
find_package(CURL REQUIRED)
fourfold_component(mail DEPS base platform LIBS CURL::libcurl)   # sending email: SMTP, Postmark, or a dev log
fourfold_component(outbound DEPS base platform compiledapp LIBS CURL::libcurl)   # outbound HTTP, secrets (overview §6)
if(TARGET test_outbound)
    find_package(ZLIB REQUIRED)   # the tests make compressed responses
    target_link_libraries(test_outbound PRIVATE ZLIB::ZLIB)
endif()
fourfold_component(script DEPS base partition platform template markdown compiledapp appdata outbound
    LIBS fourfold::ms2)                                 # the script thread; the only user of MS2
fourfold_component(config DEPS base partition compiledapp appdata script state mail outbound)   # private/config/ → AppConfig
fourfold_component(compiler DEPS base partition template markdown compiledapp script)

# --- Engine ---
fourfold_component(http DIR engine/http)           # HttpRequest/HttpResponse
fourfold_component(observer DIR engine/observer DEPS base partition compiledapp script)   # events for the dev tools
fourfold_component(policy DIR engine/policy DEPS base platform compiledapp script state)   # access, CSRF, rate limits, spam checks
fourfold_component(push DIR engine/push DEPS base platform http)   # SSE channels and subscribers (overview §7)
fourfold_component(pipeline DIR engine/pipeline DEPS base partition platform template compiledapp state appdata script config http observer policy push)
fourfold_embed(pipeline kHtmxLibrary ${CMAKE_SOURCE_DIR}/third_party/htmx/htmx.min.js)
fourfold_embed(pipeline kHtmxConfig ${CMAKE_SOURCE_DIR}/src/engine/pipeline/assets/htmx-config.js)
fourfold_embed(pipeline kMarkdownCss ${CMAKE_SOURCE_DIR}/src/engine/pipeline/assets/markdown.css)
fourfold_embed(pipeline kChallengeScript ${CMAKE_SOURCE_DIR}/src/engine/pipeline/assets/challenge.js)
fourfold_embed(pipeline kHtmxSse ${CMAKE_SOURCE_DIR}/third_party/htmx/sse.js)
fourfold_component(authflow DIR engine/authflow   # the /__auth/ pages: magic links, sign-out, fake login
    DEPS base platform template compiledapp config mail state http pipeline)
fourfold_component(lifecycle DIR engine/lifecycle   # watch, compile, migrate, swap; backups
    DEPS base partition compiledapp appdata script compiler config observer pipeline platform)
fourfold_component(jobs DIR engine/jobs   # the schedule and the job worker loop (overview §2)
    DEPS base platform compiledapp script state config observer pipeline)

# --- Hosts ---
fourfold_component(devtools DEPS base partition template compiledapp script http observer pipeline platform push)
fourfold_component(httpserver DIR hosts/http-server DEPS base platform http LIBS llhttp)
fourfold_executable(fourfold DIR hosts/cli
    DEPS base partition script compiler config mail observer pipeline authflow lifecycle devtools platform httpserver state
         appdata jobs push)
