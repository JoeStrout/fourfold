#!/bin/bash
# Runs tests/apache/apache_test.sh's suEXEC modes (CGI and mod_fcgid, each
# request run as the site's own user, as on shared hosting) in a Debian
# container, with the static Linux build (tools/linux/build-static.sh).
#
#   tools/linux/suexec-test.sh [suexec-cgi|suexec-fcgid]   (default: both)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
binary="$repo/build/linux-static/fourfold"
[[ -x "$binary" ]] || { echo "suexec-test: build it first: tools/linux/build-static.sh"; exit 1; }
modes=("$@")
[[ ${#modes[@]} -gt 0 ]] || modes=(suexec-cgi suexec-fcgid)

image=fourfold-suexec-test
docker build --platform linux/amd64 -q -t "$image" -f "$here/Dockerfile.suexec" "$here" >/dev/null
status=0
for mode in "${modes[@]}"; do
    docker run --rm --platform linux/amd64 \
        -v "$repo/tests/apache:/tests/apache:ro" -v "$binary:/opt/fourfold:ro" \
        -e FOURFOLD=/opt/fourfold -e HTTPD=/usr/sbin/apache2 -e APACHE_MODULES=/usr/lib/apache2/modules \
        "$image" bash -c "/tests/apache/apache_test.sh $mode; s=\$?; if [ \$s != 0 ]; then echo '--- suexec.log'; tail -20 /var/log/apache2/suexec.log 2>/dev/null; fi; exit \$s" \
        || status=1
done
exit $status
