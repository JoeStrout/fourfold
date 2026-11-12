#!/bin/bash
# Fourfold under a real Apache (M10; overview §8, Hosting modes): CGI through
# the entry point and .htaccess that `fourfold cgi-setup` writes, and
# FastCGI through mod_proxy_fcgi.
#
# Runs Apache as you, on a high port, from a config of its own in a
# temporary folder: nothing of the system's Apache setup is used or changed.
# Works with the Apache that comes with macOS, and with Debian's.
#
#   tests/apache/apache_test.sh [cgi|fastcgi|all]
#
# Two more modes set Apache up as shared hosting does: suEXEC, running each
# request as the site's own user, with CGI (suexec-cgi) or mod_fcgid
# (suexec-fcgid). They need root and Debian's apache2-suexec-custom and
# libapache2-mod-fcgid, so they run in a container: tools/linux/suexec-test.sh.
#
# Environment: FOURFOLD (the binary; default build/dev/bin/fourfold),
# HTTPD (default: httpd or apache2 on the PATH, or /usr/sbin/httpd),
# APACHE_MODULES (the modules folder; found if not given), PORT (18090).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
FOURFOLD="${FOURFOLD:-$repo/build/dev/bin/fourfold}"
PORT="${PORT:-18090}"
modes="${1:-all}"

if [[ -z "${HTTPD:-}" ]]; then
    for h in httpd apache2 /usr/sbin/httpd /usr/sbin/apache2; do
        if command -v "$h" >/dev/null 2>&1; then HTTPD="$(command -v "$h")"; break; fi
    done
fi
if [[ -z "${HTTPD:-}" ]]; then echo "apache_test: no httpd found; skipped"; exit 0; fi
if [[ -z "${APACHE_MODULES:-}" ]]; then
    for m in /usr/libexec/apache2 /usr/lib/apache2/modules /usr/lib64/httpd/modules /usr/local/opt/httpd/lib/httpd/modules; do
        if [[ -f "$m/mod_cgi.so" || -f "$m/mod_cgid.so" ]]; then APACHE_MODULES="$m"; break; fi
    done
fi
[[ -n "${APACHE_MODULES:-}" ]] || { echo "apache_test: can't find Apache's modules; set APACHE_MODULES"; exit 1; }
[[ -x "$FOURFOLD" ]] || { echo "apache_test: no fourfold binary at $FOURFOLD"; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/fourfold-apache.XXXXXX")"
work="$(cd "$work" && pwd -P)"
pid=""
fcgi_pid=""
# Where the app and the web root are, and who owns them (suEXEC modes: the
# site's user, who must run run-jobs too, as cron would).
app="$work/app"
webroot="$work/public_html"
owner=""
binary="$FOURFOLD"
as_owner() { if [[ -n "$owner" ]]; then su "$owner" -s /bin/sh -c "$*"; else sh -c "$*"; fi; }
fix_owner() { if [[ -n "$owner" ]]; then chown -R "$owner:$owner" "$app/code"; fi; }
cleanup() {
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    [[ -n "$fcgi_pid" ]] && kill "$fcgi_pid" 2>/dev/null || true
    sleep 0.2
    if [[ "${KEEP:-}" == "1" ]]; then echo "kept $work"; else rm -rf "${work:?}"; fi
}
trap cleanup EXIT

failures=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; failures=$((failures + 1)); }
check() {   # check NAME HAYSTACK NEEDLE
    if [[ "$2" == *"$3"* ]]; then pass "$1"; else fail "$1 (wanted '$3' in: ${2:0:300})"; fi
}
url="http://127.0.0.1:$PORT"
jar="$work/cookies.txt"
fetch() { curl -s -b "$jar" -c "$jar" "$@"; }
status() { curl -s -o /dev/null -w '%{http_code}' -b "$jar" -c "$jar" "$@"; }

# A module, if this Apache has it as a module (not built in).
load() {
    local name="$1" file="$2"
    if [[ -f "$APACHE_MODULES/$file" ]] && ! "$HTTPD" -l 2>/dev/null | grep -q "${file%.so}.c"; then
        echo "LoadModule $name $APACHE_MODULES/$file"
    fi
}

start_apache() {   # start_apache EXTRA-CONFIG
    local conf="$work/httpd.conf"
    {
        for mpm in mpm_prefork mpm_event mpm_worker; do
            if [[ -f "$APACHE_MODULES/mod_$mpm.so" ]]; then load "${mpm}_module" "mod_$mpm.so"; break; fi
        done
        load authz_core_module mod_authz_core.so
        load authz_host_module mod_authz_host.so
        load mime_module mod_mime.so
        load log_config_module mod_log_config.so
        load env_module mod_env.so
        load dir_module mod_dir.so
        load unixd_module mod_unixd.so
        load rewrite_module mod_rewrite.so
        if [[ -f "$APACHE_MODULES/mod_cgid.so" ]] && ! [[ -f "$APACHE_MODULES/mod_mpm_prefork.so" ]]; then
            load cgid_module mod_cgid.so
        else
            load cgi_module mod_cgi.so
        fi
        echo "$1"
        cat <<EOF
ServerName 127.0.0.1
Listen 127.0.0.1:$PORT
PidFile $work/httpd.pid
DefaultRuntimeDir $work
Mutex file:$work default
ErrorLog $work/error.log
LogLevel warn
LogFormat "%h \"%r\" %>s %b" common
CustomLog $work/access.log common
<IfModule cgid_module>
    ScriptSock $work/cgisock
</IfModule>
TypesConfig /dev/null
DocumentRoot "$webroot"
<Directory "$webroot">
    AllowOverride All
    Require all granted
</Directory>
EOF
    } > "$conf"
    # In a process group of its own: Apache signals its whole group on stopping.
    set -m
    "$HTTPD" -f "$conf" -DFOREGROUND &
    pid=$!
    set +m
    for _ in $(seq 1 50); do
        if curl -s -o /dev/null "$url/" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "apache_test: Apache didn't start:"; cat "$work/error.log"; exit 1
}

stop_apache() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    pid=""
}

# The checks every mode passes.
exercise() {
    rm -f "$jar"
    local body
    body="$(fetch "$url/")"
    check "a page, with its .ms file's fields" "$body" "hello from version one"
    check "the engine's headers" "$(curl -sI "$url/")" "Content-Security-Policy"
    check "a static file, through Fourfold" "$(curl -sI "$url/style.css")" "X-Content-Type-Options: nosniff"
    check "Markdown" "$(fetch "$url/about")" "A page in Markdown"
    check "Markdown as Markdown" "$(fetch "$url/about.md")" "# About"
    check "an unknown page is Fourfold's 404" "$(status "$url/nowhere")" "404"
    [[ "$(fetch "$url/fourfold.cgi")$(fetch "$url/fourfold.fcgi")" != *"exec "* ]] && pass "the entry point's source stays hidden" || fail "entry point source served"
    [[ "$(fetch "$url/.htaccess")" != *"RewriteRule"* ]] && pass ".htaccess stays hidden" || fail ".htaccess served"

    rm -f "$jar"
    check "a session, request 1" "$(fetch "$url/count/up")" "1"
    check "a session, request 2" "$(fetch "$url/count/up")" "2"
    check "a session, request 3" "$(fetch "$url/count/up")" "3"

    # A form post, with the CSRF token from the page.
    rm -f "$jar"
    body="$(fetch "$url/")"
    local token
    token="$(sed -n 's/.*name="_csrf" value="\([^"]*\)".*/\1/p' <<<"$body" | head -1)"
    [[ -n "$token" ]] && pass "the form has a CSRF token" || fail "no CSRF token in the page"
    check "a form post" "$(fetch -X POST -H "Origin: $url" --data-urlencode "_csrf=$token" --data-urlencode "name=Ada" "$url/guest/sign")" "signed by Ada"
    check "the post was stored" "$(fetch "$url/guest/last")" "Ada"
    check "a post without the token is refused" "$(status -X POST -H "Origin: $url" --data "name=Eve" "$url/guest/sign")" "403"

    check "a webhook gets Authorization" "$(fetch -X POST -H 'Authorization: Bearer s3cret' --data '{}' "$url/hook/ping")" "auth: Bearer s3cret"

    # Jobs: queued by a request, run by run-jobs (as cron would).
    check "a job is queued" "$(fetch "$url/work/queue?t=from-apache")" "queued"
    as_owner "'$binary' run-jobs '$app'" && pass "run-jobs" || fail "run-jobs failed"
    check "the job ran" "$(fetch "$url/work/notes")" "from-apache"

    # A deploy: new code, then .deploy.
    sed -i.bak 's/version one/version two/' "$app/code/pages/index.ms"
    fix_owner
    touch "$app/code/.deploy"
    check "a deploy is served at once" "$(fetch "$url/")" "hello from version two"
    sed -i.bak 's/version two/version one/' "$app/code/pages/index.ms"
    rm -f "$app/code/pages/index.ms.bak"
    fix_owner
    touch "$app/code/.deploy"
    check "and back again" "$(fetch "$url/")" "hello from version one"

    # A broken deploy keeps the last good version.
    echo 'render = function(req' > "$app/code/pages/index.ms"
    fix_owner
    touch "$app/code/.deploy"
    check "a broken deploy keeps serving the old version" "$(fetch "$url/")" "hello from version one"
    printf 'render = function(req)\n    return {"where": "version one"}\nend function\n' > "$app/code/pages/index.ms"
    fix_owner
    touch "$app/code/.deploy"
    check "fixed, it's served" "$(fetch "$url/")" "hello from version one"

    # Many at once: one session, from 8 concurrent clients.
    rm -f "$jar"
    fetch "$url/count/up" >/dev/null
    local cookie
    cookie="$(awk '$6 ~ /session/ {print $6"="$7}' "$jar")"
    for i in $(seq 1 8); do
        (for j in $(seq 1 10); do curl -s -o /dev/null -w '%{http_code}\n' -H "Cookie: $cookie" "$url/count/up"; done) > "$work/load$i.txt" &
    done
    sleep 0.2
    while [[ "$(cat "$work"/load*.txt | wc -l | tr -d ' ')" -lt 80 ]]; do sleep 0.1; done
    local oks
    oks="$(cat "$work"/load*.txt | grep -c '^200$' || true)"
    [[ "$oks" == "80" ]] && pass "80 concurrent requests, all 200" || fail "concurrent requests: $oks of 80 were 200"
    check "the session counted every one" "$(curl -s -H "Cookie: $cookie" "$url/count/up")" "82"
    rm -f "$work"/load*.txt
}

# --- Set up: the app, and its web root, as cgi-setup makes them ---
if [[ "$modes" != suexec-* ]]; then cp -R "$here/app" "$app"; fi

if [[ "$modes" == "cgi" || "$modes" == "all" ]]; then
    echo "CGI ($HTTPD)"
    "$FOURFOLD" cgi-setup "$app" "$webroot" --binary "$FOURFOLD" > "$work/setup.txt"
    pass "cgi-setup"
    start_apache ""
    exercise
    # The app folder in the web root is refused, by the entry point too.
    stop_apache
    echo
fi

if [[ "$modes" == "fastcgi" || "$modes" == "all" ]]; then
    if [[ ! -f "$APACHE_MODULES/mod_proxy_fcgi.so" ]]; then
        echo "FastCGI: no mod_proxy_fcgi here; skipped"
    elif ! "$FOURFOLD" help | grep -q fastcgi; then
        echo "FastCGI: this fourfold has no fastcgi command yet; skipped"
    else
        echo "FastCGI (mod_proxy_fcgi, $HTTPD)"
        rm -rf "${webroot:?}"
        "$FOURFOLD" cgi-setup "$app" "$webroot" --binary "$FOURFOLD" --fastcgi > "$work/setup.txt"
        # On a TCP port: a Unix socket's path can't be long, and $TMPDIR's may be.
        fcgi_port=$((PORT + 1))
        "$FOURFOLD" fastcgi "$app" --listen "127.0.0.1:$fcgi_port" 2> "$work/fastcgi.log" &
        fcgi_pid=$!
        sleep 0.5
        # mod_proxy_fcgi instead of mod_fcgid: everything goes to the socket.
        start_apache "$(load proxy_module mod_proxy.so)
$(load proxy_fcgi_module mod_proxy_fcgi.so)
<LocationMatch \"^/\">
    SetHandler \"proxy:fcgi://127.0.0.1:$fcgi_port/\"
</LocationMatch>
ProxyFCGIBackendType GENERIC
"
        exercise
        stop_apache
    fi
fi

if [[ "$modes" == suexec-* ]]; then
    # As on shared hosting: the site is a user of its own; Apache runs as
    # www-data, and suEXEC switches to the site's user for each request.
    [[ "$(id -u)" == 0 ]] || { echo "apache_test: $modes needs root (run tools/linux/suexec-test.sh)"; exit 1; }
    owner=site
    id "$owner" >/dev/null 2>&1 || useradd -m -s /bin/sh "$owner"
    home="$(getent passwd "$owner" | cut -d: -f6)"
    app="$home/apps/demo"
    webroot="/var/www/site"
    binary="$home/bin/fourfold"
    rm -rf "${app:?}" "${webroot:?}"
    mkdir -p "$home/bin" "$home/apps" "$webroot"
    install -m 755 "$FOURFOLD" "$binary"
    cp -R "$here/app" "$app"
    chown -R "$owner:$owner" "$home" "$webroot"
    chmod 755 "$home" "$webroot"
    # suexec-custom: requests may run programs under /var/www.
    mkdir -p /etc/apache2/suexec
    printf '/var/www\npublic_html/cgi-bin\n' > /etc/apache2/suexec/www-data
    chmod 755 "$work"
    fastcgi_flag=""
    handler=""   # mod_cgi: start_apache loads it
    if [[ "$modes" == suexec-fcgid ]]; then
        fastcgi_flag="--fastcgi"
        handler="$(load fcgid_module mod_fcgid.so)
FcgidIPCDir $work/fcgid
FcgidProcessTableFile $work/fcgid/table"
        mkdir -p "$work/fcgid"
        chown www-data "$work/fcgid"
    fi
    echo "suEXEC, ${modes#suexec-} ($HTTPD)"
    as_owner "'$binary' cgi-setup '$app' '$webroot' $fastcgi_flag" > "$work/setup.txt" && pass "cgi-setup, as the site's user" || fail "cgi-setup"
    start_apache "$(load suexec_module mod_suexec.so)
$handler
User www-data
Group www-data
SuexecUserGroup $owner $owner
"
    exercise
    if [[ "$modes" == suexec-fcgid ]]; then
        # mod_fcgid keeps fourfold processes running, as the site's user.
        procs="$(pgrep -u "$owner" -f "fourfold fastcgi" | wc -l | tr -d ' ')"
        [[ "$procs" -ge 1 ]] && pass "mod_fcgid kept $procs fourfold fastcgi process(es), as the site's user" \
            || fail "no fourfold fastcgi process is running as $owner"
    fi
    # Every process ran as the site's user: nothing it made belongs to anyone else.
    others="$(find "$app" ! -user "$owner" | head -5)"
    [[ -z "$others" ]] && pass "everything in the app folder belongs to the site's user" || fail "files not the site's: $others"
    stop_apache
fi

echo
if [[ "$failures" -gt 0 ]]; then
    echo "apache_test: $failures failed (Apache's error log: $work/error.log; run with KEEP=1 to keep it)"
    exit 1
fi
echo "apache_test: all passed"
