#!/bin/sh
# M11.1's isolation test, in a Debian container with real OS users (no
# systemd: the processes are started by hand, as the units would start
# them). Two apps, a and b, behind fourfold front; then everything one
# app's engine (or its dropbox writer) might try against the other app, or
# against its own protected partitions, done as that user.
#
#   tools/server/isolation-test.sh [--binary PATH]
#
# Needs Docker, and the static Linux binary (tools/linux/build-static.sh).
set -eu
cd "$(dirname "$0")/../.."
binary=build/linux-static/fourfold
[ "${1:-}" = "--binary" ] && binary=$2
[ -x "$binary" ] || { echo "no binary at $binary (run tools/linux/build-static.sh)" >&2; exit 1; }

docker run --rm --platform linux/amd64 \
    -v "$PWD/$binary:/usr/local/bin/fourfold:ro" \
    -v "$PWD/tools/server:/opt/fourfold-server:ro" \
    debian:12 sh -euc '
apt-get update -qq >/dev/null && apt-get install -y -qq curl openssl >/dev/null 2>&1

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok:   $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL: $1"; }
# expect_denied USER DESCRIPTION COMMAND...: the command must fail.
expect_denied() {
    u=$1; what=$2; shift 2
    if runuser -u "$u" -- "$@" >/dev/null 2>&1; then bad "$u could $what"; else ok "$u can'"'"'t $what"; fi
}
expect_allowed() {
    u=$1; what=$2; shift 2
    if runuser -u "$u" -- "$@" >/dev/null 2>&1; then ok "$u can $what"; else bad "$u couldn'"'"'t $what"; fi
}

useradd --create-home dev
for app in a b; do
    mkdir -p /srv/$app/code/pages /srv/$app/private/config
    cat > /srv/$app/code/pages/t.ms <<EOF
get.hi = function(req)
    dropbox.write "visits.log", "visit to $app from " + req.ip
    return "hello from $app, you are " + req.ip
end function
EOF
    printf "config = {}\nconfig.domain = \"%s.test\"\n" $app > /srv/$app/private/config/app.ms
    printf "secrets = {}\nsecrets.key = {\"value\": \"secret-of-%s\"}\n" $app > /srv/$app/private/config/secrets.ms
    sh /opt/fourfold-server/fourfold-add-app $app --domain $app.test --developer dev
done
echo "== layout"
ls -ld /srv/a /srv/a/* /srv/a/private/*

# front: its own certificates for the test names.
mkdir -p /etc/fourfold/tls /run/fourfold /var/lib/fourfold-front
for app in a b; do
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 30 \
        -subj "/CN=$app.test" -addext "subjectAltName=DNS:$app.test" \
        -keyout /etc/fourfold/tls/$app.key -out /etc/fourfold/tls/$app.crt 2>/dev/null
    sed -i "s|^server.apps\[\"$app\"\] = {\"domains\": \[\"$app.test\"\]}|server.apps[\"$app\"] = {\"domains\": [\"$app.test\"], \"certFile\": \"/etc/fourfold/tls/$app.crt\", \"keyFile\": \"/etc/fourfold/tls/$app.key\"}|" /etc/fourfold/server.ms
done
chown -R fourfold-front:fourfold-front /etc/fourfold/tls /var/lib/fourfold-front
chmod 0700 /etc/fourfold/tls
# Each user makes its own socket here; the sticky bit keeps them from
# removing the sockets of others, and no one may list it.
chmod 1733 /run/fourfold
echo "== /etc/fourfold/server.ms"; cat /etc/fourfold/server.ms

# Start everything as the units would. Without systemd, root hands each
# socket to the group that may connect (as SocketGroup= does).
waitfor() { for i in $(seq 1 50); do [ -S "$1" ] && return 0; sleep 0.1; done; echo "no socket $1"; cat /tmp/*.log; exit 1; }
for app in a b; do
    runuser -u ff-$app-log -- sh -c "umask 027; exec fourfold dropbox-writer /srv/$app/dropbox --listen /run/fourfold/$app-dropbox.sock" \
        > /tmp/$app-writer.log 2>&1 &
    waitfor /run/fourfold/$app-dropbox.sock
    chgrp ff-$app /run/fourfold/$app-dropbox.sock
    runuser -u ff-$app -- sh -c "umask 007; exec fourfold serve /srv/$app --listen unix:/run/fourfold/$app.sock \
        --server-config /etc/fourfold/server.ms --dropbox-socket /run/fourfold/$app-dropbox.sock" > /tmp/$app-engine.log 2>&1 &
    waitfor /run/fourfold/$app.sock
    chgrp fourfold-front /run/fourfold/$app.sock
done
runuser -u fourfold-front -- fourfold front --server-config /etc/fourfold/server.ms --state /var/lib/fourfold-front \
    --listen 127.0.0.1:8443 --redirect-listen 127.0.0.1:8080 > /tmp/front.log 2>&1 &
sleep 2
cat /tmp/front.log /tmp/a-engine.log

echo "== through front"
for app in a b; do
    got=$(curl -sk --resolve $app.test:8443:127.0.0.1 https://$app.test:8443/t/hi)
    case "$got" in "hello from $app, you are 127.0.0.1") ok "$app.test answers: $got" ;; *) bad "$app.test: $got" ;; esac
done
code=$(curl -sk -o /dev/null -w "%{http_code}" --resolve c.test:8443:127.0.0.1 https://c.test:8443/)
[ "$code" = 421 ] && ok "an unknown name gets 421" || bad "an unknown name got $code"
loc=$(curl -s -o /dev/null -w "%{redirect_url}" -H "Host: b.test" http://127.0.0.1:8080/x)
[ "$loc" = "https://b.test:8443/x" ] && ok "port 80 redirects to $loc" || bad "redirect: $loc"
sleep 0.3
if grep -q "visit to a from 127.0.0.1" /srv/a/dropbox/visits.log; then ok "a'"'"'s dropbox line arrived"; else bad "no dropbox line for a"; fi
stat -c "  %U:%G %a %n" /srv/a/dropbox/visits.log || true

echo "== app a'"'"'s engine (ff-a), as if compromised"
expect_denied ff-a "read b'"'"'s data"            ls /srv/b/data
expect_denied ff-a "read b'"'"'s secrets"         cat /srv/b/private/config/secrets.ms
expect_denied ff-a "read b'"'"'s code"            cat /srv/b/code/pages/t.ms
expect_denied ff-a "read b'"'"'s state"           ls /srv/b/private/state
expect_denied ff-a "read b'"'"'s dropbox"         cat /srv/b/dropbox/visits.log
expect_denied ff-a "connect to b'"'"'s engine"    curl -s --max-time 2 --unix-socket /run/fourfold/b.sock http://b.test/
expect_denied ff-a "connect to b'"'"'s dropbox writer" curl -s --max-time 2 --http0.9 --unix-socket /run/fourfold/b-dropbox.sock http://x/
expect_denied ff-a "write its own code"         sh -c "echo x >> /srv/a/code/pages/t.ms"
expect_denied ff-a "add to its own code"        touch /srv/a/code/pages/new.ms
expect_denied ff-a "change its own config"      sh -c "echo x >> /srv/a/private/config/app.ms"
expect_denied ff-a "read its own dropbox"       cat /srv/a/dropbox/visits.log
expect_denied ff-a "list its own dropbox"       ls /srv/a/dropbox
expect_denied ff-a "truncate its own dropbox"   sh -c ": > /srv/a/dropbox/visits.log"
expect_denied ff-a "remove its own dropbox file" rm -f /srv/a/dropbox/visits.log
expect_denied ff-a "read front'"'"'s keys"         cat /etc/fourfold/tls/a.key
expect_denied ff-a "change the server config"   sh -c "echo x >> /etc/fourfold/server.ms"
expect_allowed ff-a "read its own code"         cat /srv/a/code/pages/t.ms
expect_allowed ff-a "read its own secrets"      cat /srv/a/private/config/secrets.ms
expect_allowed ff-a "write its own data"        touch /srv/a/data/x
expect_allowed ff-a "write its own state"       touch /srv/a/private/state/x
expect_allowed ff-a "send to its dropbox writer" curl -s --max-time 2 --http0.9 --unix-socket /run/fourfold/a-dropbox.sock http://x/ -o /dev/null

echo "== app a'"'"'s dropbox writer (ff-a-log)"
expect_denied ff-a-log "read a'"'"'s data"        ls /srv/a/data
expect_denied ff-a-log "read a'"'"'s secrets"     cat /srv/a/private/config/secrets.ms
expect_denied ff-a-log "read a'"'"'s code"        ls /srv/a/code
expect_denied ff-a-log "connect to a'"'"'s engine" curl -s --max-time 2 --unix-socket /run/fourfold/a.sock http://a.test/t/hi
expect_denied ff-a-log "read b'"'"'s dropbox"     cat /srv/b/dropbox/visits.log

echo "== front (fourfold-front)"
expect_denied fourfold-front "read a'"'"'s code"     ls /srv/a/code
expect_denied fourfold-front "read a'"'"'s data"     ls /srv/a/data
expect_denied fourfold-front "read a'"'"'s secrets"  cat /srv/a/private/config/secrets.ms
expect_denied fourfold-front "read a'"'"'s dropbox"  ls /srv/a/dropbox
expect_denied fourfold-front "connect to a'"'"'s dropbox writer" curl -s --max-time 2 --http0.9 --unix-socket /run/fourfold/a-dropbox.sock http://x/
expect_allowed fourfold-front "connect to a'"'"'s engine" curl -s --max-time 2 --unix-socket /run/fourfold/a.sock -H "Host: a.test" http://a.test/t/hi

echo "== the developer (dev)"
expect_allowed dev "edit a'"'"'s code"           sh -c "echo >> /srv/a/code/pages/t.ms"
expect_allowed dev "edit a'"'"'s config"         sh -c "echo >> /srv/a/private/config/app.ms"
expect_allowed dev "read a'"'"'s dropbox"        cat /srv/a/dropbox/visits.log
expect_allowed dev "read a'"'"'s data"           ls /srv/a/data
expect_denied dev "write a'"'"'s dropbox"        sh -c "echo x >> /srv/a/dropbox/visits.log"

echo
echo "isolation: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "-- logs"; tail -5 /tmp/*.log; exit 1; }
'
