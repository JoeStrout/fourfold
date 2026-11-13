#!/bin/sh
# Deploys a Fourfold app to a Linux server with systemd, as the admin guide
# describes: the static binary in /usr/local/bin, the app in /srv/<name>,
# owned by an unprivileged user "fourfold", with systemd opening ports 443
# and 80 (socket activation) and the firewall (ufw) letting them in.
#
#   tools/vps/deploy.sh [options] user@host domain app-folder
#
#   --name NAME     the app's name on the server (default: the folder's name)
#   --staging       use Let's Encrypt's staging CA (untrusted certificates)
#   --memory SIZE   systemd MemoryMax (default 700M)
#   --binary PATH   the Linux binary (default build/linux-static/fourfold;
#                   tools/linux/build-static.sh makes it)
#
# Run it again to deploy changes: the binary and code/ are replaced; data/,
# dropbox/ and private/ on the server are kept (data/ is copied only the first
# time), and private/config/app.ms is written only if it isn't there.
# The ssh user needs sudo without a password.
set -eu

cd "$(dirname "$0")/../.."
name=""; staging=0; memory=700M; binary=build/linux-static/fourfold
while [ $# -gt 0 ]; do
    case "$1" in
        --name) name=$2; shift 2 ;;
        --staging) staging=1; shift ;;
        --memory) memory=$2; shift 2 ;;
        --binary) binary=$2; shift 2 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -eq 3 ] || { sed -n '6,13p' "$0" >&2; exit 2; }
target=$1; domain=$2; app=${3%/}
[ -n "$name" ] || name=$(basename "$app")
case "$name" in *[!a-z0-9-]*|"") echo "name must be lowercase letters, digits, hyphens: $name" >&2; exit 2 ;; esac
[ -x "$binary" ] || { echo "no binary at $binary (run tools/linux/build-static.sh)" >&2; exit 1; }
[ -d "$app/code" ] || { echo "$app has no code/ folder" >&2; exit 1; }

acme=""
[ $staging -eq 1 ] && acme='config.tls = {"acme": "letsencrypt-staging"}'

echo "== uploading"
remote=/tmp/fourfold-deploy-$name
ssh "$target" "rm -rf $remote && mkdir -p $remote"
scp -q "$binary" "$target:$remote/fourfold"
COPYFILE_DISABLE=1 tar -C "$app" --no-xattrs -czf - --exclude private --exclude .DS_Store code data dropbox 2>/dev/null \
    | ssh "$target" "tar -C $remote -xzf -"

echo "== installing"
ssh "$target" sudo sh -s "$name" "$domain" "$memory" "$remote" "'$acme'" <<'REMOTE'
set -eu
name=$1; domain=$2; memory=$3; remote=$4; acme=$5
srv=/srv/$name
unit=fourfold-$name

id fourfold >/dev/null 2>&1 || useradd --system --home-dir /srv --shell /usr/sbin/nologin fourfold

install -m 0755 "$remote/fourfold" /usr/local/bin/fourfold.new
mv /usr/local/bin/fourfold.new /usr/local/bin/fourfold

mkdir -p "$srv/private/config" "$srv/private/state" "$srv/data" "$srv/dropbox"
rm -rf "$srv/code.new"
mv "$remote/code" "$srv/code.new"
rm -rf "$srv/code.old"
[ -d "$srv/code" ] && mv "$srv/code" "$srv/code.old"
mv "$srv/code.new" "$srv/code"
rm -rf "$srv/code.old"
if [ -z "$(ls -A "$srv/data")" ] && [ -d "$remote/data" ]; then cp -a "$remote/data/." "$srv/data/"; fi
if [ ! -f "$srv/private/config/app.ms" ]; then
    printf 'config = {}\nconfig.domain = "%s"\n%s\n' "$domain" "$acme" > "$srv/private/config/app.ms"
fi
chown -R fourfold:fourfold "$srv"
chmod 0750 "$srv" "$srv/private"
rm -rf "$remote"

cat > /etc/systemd/system/$unit.socket <<EOF
[Unit]
Description=Fourfold's ports: $domain

[Socket]
# In this order: HTTPS first, then port 80.
ListenStream=443
ListenStream=80

[Install]
WantedBy=sockets.target
EOF

cat > /etc/systemd/system/$unit.service <<EOF
[Unit]
Description=Fourfold: $domain
After=network.target
Requires=$unit.socket

[Service]
User=fourfold
ExecStart=/usr/local/bin/fourfold serve $srv --tls
Restart=on-failure
MemoryMax=$memory
LimitNOFILE=65536
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=$srv
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF

ufw allow 80/tcp >/dev/null
ufw allow 443/tcp >/dev/null

systemctl daemon-reload
systemctl enable --quiet $unit.socket $unit.service
systemctl start $unit.socket
systemctl restart $unit.service
sleep 3
systemctl --no-pager --lines=0 status $unit.service | head -5
journalctl -u $unit.service --no-pager -n 25 -o cat
REMOTE
