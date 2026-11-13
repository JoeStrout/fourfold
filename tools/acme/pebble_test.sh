#!/bin/bash
# Gets a real certificate through ACME, from Pebble (Let's Encrypt's test
# CA) in Docker, with `fourfold serve --tls`, then fetches a page over HTTPS
# and HTTP/2 with curl, trusting Pebble's root.
#
#   tools/acme/pebble_test.sh [path/to/fourfold]
#
# Needs Docker (Desktop: host.docker.internal) and curl. Pebble checks the
# HTTP-01 challenge by connecting to port 5002 of the name asked for.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
fourfold="${1:-$repo/build/dev/bin/fourfold}"
work="$(mktemp -d)"
net=fourfold-acme
https_port=18443
# Docker resolves this name to this machine, so Pebble reaches port 5002 here.
domain=host.docker.internal

cleanup() {
    [[ -n "${server:-}" ]] && kill "$server" 2>/dev/null && wait "$server" 2>/dev/null || true
    docker rm -f ff-pebble >/dev/null 2>&1 || true
    docker network rm "$net" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

docker network create "$net" >/dev/null 2>&1 || true
docker run -d --name ff-pebble --network "$net" -p 14000:14000 -p 15000:15000 \
    -e PEBBLE_VA_NOSLEEP=1 \
    ghcr.io/letsencrypt/pebble:latest >/dev/null

# Pebble's own HTTPS certificate is signed by its test "minica" root.
for i in $(seq 1 50); do
    docker cp ff-pebble:/test/certs/pebble.minica.pem "$work/minica.pem" 2>/dev/null && break
    sleep 0.2
done
for i in $(seq 1 50); do
    curl -sf --cacert "$work/minica.pem" https://localhost:14000/dir >/dev/null && break
    sleep 0.2
done

mkdir -p "$work/app/code/pages" "$work/app/private/config"
echo '<!DOCTYPE html><html><body><h1>Hello over ACME</h1></body></html>' > "$work/app/code/pages/index.html"
cat > "$work/app/private/config/app.ms" <<EOF
config = {}
config.domain = "$domain"
config.tls = {"acme": "https://localhost:14000/dir", "acmeCaBundle": "$work/minica.pem", "email": "test@example.com"}
EOF

"$fourfold" serve "$work/app" --tls --listen "127.0.0.1:$https_port" --redirect-listen "0.0.0.0:5002" \
    > "$work/out.log" 2>&1 &
server=$!
for i in $(seq 1 150); do
    grep -q "got a certificate" "$work/out.log" && break
    grep -q "couldn't get a certificate" "$work/out.log" && break
    sleep 0.2
done
cat "$work/out.log"
grep -q "got a certificate" "$work/out.log" || { echo "FAIL: no certificate"; exit 1; }

# The issued certificate chains to Pebble's (per-run) root.
curl -sf --cacert "$work/minica.pem" https://localhost:15000/roots/0 > "$work/root.pem"
page="$(curl -sS --http2 -D "$work/headers" --cacert "$work/root.pem" \
    --resolve "$domain:$https_port:127.0.0.1" "https://$domain:$https_port/")"
echo "$page" | grep -q "Hello over ACME" || { echo "FAIL: page: $page"; exit 1; }
grep -qi "^HTTP/2 200" "$work/headers" || { echo "FAIL: not HTTP/2"; cat "$work/headers"; exit 1; }
grep -qi "^strict-transport-security:" "$work/headers" || { echo "FAIL: no HSTS"; exit 1; }
ls -l "$work/app/private/state/tls/"
echo "PASS: a certificate from Pebble, served over HTTPS and HTTP/2"
