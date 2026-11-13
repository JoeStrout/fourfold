#!/bin/sh
# Deploys one app to a server with several apps (M11.1), from this machine:
# the static binary to /usr/local/bin, this folder's scripts and units to
# /opt/fourfold-server, the app's code to /srv/NAME (as the ssh user, who is
# its developer), then fourfold-add-app to set up its users, permissions,
# server config entry and units, and (re)start it and fourfold front.
#
#   tools/server/deploy.sh [--binary PATH] [--memory SIZE] user@host NAME DOMAIN[,DOMAIN...] APP-FOLDER
#
# Run it again to deploy changes: code/ is replaced; data/, dropbox/ and
# private/ on the server are kept (data/ is copied only the first time, and
# private/config/app.ms written only if it isn't there). The ssh user needs
# sudo without a password.
set -eu

cd "$(dirname "$0")/../.."
binary=build/linux-static/fourfold; memory=""
while [ $# -gt 0 ]; do
    case "$1" in
        --binary) binary=$2; shift 2 ;;
        --memory) memory=$2; shift 2 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -eq 4 ] || { sed -n '8p' "$0" >&2; exit 2; }
target=$1; name=$2; domains=$3; app=${4%/}
[ -x "$binary" ] || { echo "no binary at $binary (run tools/linux/build-static.sh)" >&2; exit 1; }
[ -d "$app/code" ] || { echo "$app has no code/ folder" >&2; exit 1; }

echo "== uploading"
remote=/tmp/fourfold-deploy-$name
ssh "$target" "rm -rf $remote && mkdir -p $remote"
scp -q "$binary" "$target:$remote/fourfold"
COPYFILE_DISABLE=1 tar -C tools --no-xattrs -czf - server 2>/dev/null | ssh "$target" "tar -C $remote -xzf -"
COPYFILE_DISABLE=1 tar -C "$app" --no-xattrs -czf - --exclude private --exclude dropbox --exclude .DS_Store code data \
    2>/dev/null | ssh "$target" "mkdir -p $remote/app && tar -C $remote/app -xzf -"

echo "== installing"
ssh "$target" sudo sh -s "$name" "$domains" "$remote" "$(ssh "$target" whoami)" "$memory" <<'REMOTE'
set -eu
name=$1; domains=$2; remote=$3; developer=$4; memory=${5:-}
srv=/srv/$name
install -m 0755 "$remote/fourfold" /usr/local/bin/fourfold.new
mv /usr/local/bin/fourfold.new /usr/local/bin/fourfold
rm -rf /opt/fourfold-server
cp -r "$remote/server" /opt/fourfold-server
chmod 0755 /opt/fourfold-server/fourfold-add-app

args=""
for d in $(echo "$domains" | tr , ' '); do args="$args --domain $d"; done
[ -n "$memory" ] && args="$args --memory $memory"
# The folders and users first; then the code, as its developer.
sh /opt/fourfold-server/fourfold-add-app "$name" $args --developer "$developer" --no-start >/dev/null
rm -rf "$srv/code.new" "$srv/code.old"
mv "$remote/app/code" "$srv/code.new"
[ -d "$srv/code" ] && mv "$srv/code" "$srv/code.old"
mv "$srv/code.new" "$srv/code"
rm -rf "$srv/code.old"
if [ -z "$(ls -A "$srv/data")" ] && [ -d "$remote/app/data" ]; then cp -a "$remote/app/data/." "$srv/data/"; fi
first=$(echo "$domains" | cut -d, -f1)
[ -f "$srv/private/config/app.ms" ] || printf 'config = {}\nconfig.domain = "%s"\n' "$first" > "$srv/private/config/app.ms"
rm -rf "$remote"
# Ownership and permissions over everything, and (re)start.
sh /opt/fourfold-server/fourfold-add-app "$name" $args --developer "$developer"
REMOTE
