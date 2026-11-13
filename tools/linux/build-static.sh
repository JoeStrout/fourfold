#!/bin/bash
# Builds a fully static Linux x86_64 fourfold (musl, in an Alpine container),
# and runs the tests there. The result is build/linux-static/fourfold.
#
#   tools/linux/build-static.sh [--no-tests]
#
# Needs Docker. MiniScript 2 is found as for any build (FOURFOLD_MS2_DIR, or
# ../miniscript2). Postgres support is left out (SQLite only).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ms2="$(cd "${FOURFOLD_MS2_DIR:-$repo/../miniscript2}" && pwd)"
tests=ON
[[ "${1:-}" == "--no-tests" ]] && tests=OFF
out="$repo/build/linux-static"
mkdir -p "$out"

image=fourfold-static-build
docker build --platform linux/amd64 -q -t "$image" -f "$here/Dockerfile.static" "$here" >/dev/null

docker run --rm --platform linux/amd64 \
    -v "$repo:/src/fourfold:ro" -v "$ms2:/src/miniscript2:ro" -v "$out:/out" \
    -e TESTS="$tests" "$image" sh -euc '
        libs="$(pkg-config --static --libs-only-l libcurl)"
        zlib="$(ls /lib/libz.a /usr/lib/libz.a 2>/dev/null | head -1)"
        cmake -S /src/fourfold -B /build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DFOURFOLD_MS2_DIR=/src/miniscript2 \
            -DFOURFOLD_BUILD_TESTS=$TESTS \
            -DFOURFOLD_PG_CONFIG=FOURFOLD_PG_CONFIG-NOTFOUND \
            -DCURL_LIBRARY=/usr/lib/libcurl.a \
            -DOPENSSL_USE_STATIC_LIBS=ON \
            -DFOURFOLD_NGHTTP2_LIBRARY=/usr/lib/libnghttp2.a \
            -DZLIB_LIBRARY="$zlib" \
            -DCMAKE_EXE_LINKER_FLAGS="-static" \
            -DCMAKE_CXX_STANDARD_LIBRARIES="$libs" > /out/configure.log
        cmake --build /build > /out/build.log 2>&1 || { tail -40 /out/build.log; exit 1; }
        if [ "$TESTS" = ON ]; then
            (cd /build && ctest --output-on-failure > /out/test.log 2>&1) || { tail -60 /out/test.log; exit 1; }
            tail -3 /out/test.log
        fi
        strip -o /out/fourfold /build/bin/fourfold
    '
file "$out/fourfold" 2>/dev/null || true
ls -l "$out/fourfold"
