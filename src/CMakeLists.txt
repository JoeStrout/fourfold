# Fourfold's components, lowest layer first (Arch §2). Each component may
# depend only on components declared above it; the DEPS lists here are the
# whole dependency graph.

if(FOURFOLD_BUILD_TESTS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/support ${CMAKE_BINARY_DIR}/tests/support)
endif()

# --- Foundation ---
fourfold_component(base)
target_link_libraries(ff_base PRIVATE monocypher)
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
fourfold_component(appdata DEPS base partition platform state)   # the app's data: Tier 1 files (later databases)
find_package(CURL REQUIRED)
fourfold_component(mail DEPS base platform LIBS CURL::libcurl)   # sending email: SMTP, Postmark, or a dev log
fourfold_component(script DEPS base partition platform template markdown compiledapp appdata
    LIBS fourfold::ms2)                                 # the script thread; the only user of MS2
fourfold_component(config DEPS base partition compiledapp script state mail)   # private/config/ → AppConfig
fourfold_component(compiler DEPS base partition template markdown compiledapp script)

# --- Engine ---
fourfold_component(http DIR engine/http)           # HttpRequest/HttpResponse
fourfold_component(observer DIR engine/observer DEPS base partition compiledapp script)   # events for the dev tools
fourfold_component(policy DIR engine/policy DEPS base platform compiledapp script)   # CSRF; later access and limits
fourfold_component(pipeline DIR engine/pipeline DEPS base partition platform template compiledapp state appdata script config http observer policy)
fourfold_embed(pipeline kHtmxLibrary ${CMAKE_SOURCE_DIR}/third_party/htmx/htmx.min.js)
fourfold_embed(pipeline kHtmxConfig ${CMAKE_SOURCE_DIR}/src/engine/pipeline/assets/htmx-config.js)
fourfold_embed(pipeline kMarkdownCss ${CMAKE_SOURCE_DIR}/src/engine/pipeline/assets/markdown.css)
fourfold_component(authflow DIR engine/authflow   # the /__auth/ pages: magic links, sign-out, fake login
    DEPS base platform template compiledapp config mail state http pipeline)
fourfold_component(lifecycle DIR engine/lifecycle   # watch, compile, swap
    DEPS base partition compiledapp script compiler config observer pipeline platform)

# --- Hosts ---
fourfold_component(devtools DEPS base partition template compiledapp script http observer pipeline platform)
fourfold_component(httpserver DIR hosts/http-server DEPS base platform http LIBS llhttp)
fourfold_executable(fourfold DIR hosts/cli
    DEPS base partition script compiler config mail observer pipeline authflow lifecycle devtools platform httpserver state
         appdata)
