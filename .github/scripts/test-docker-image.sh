#!/usr/bin/env bash
# test-docker-image.sh <image> -- check what the ngPost container image promises,
# on the image itself.
#
# docker.yml runs it on every change and every week; release.yml runs it on the
# image it is about to publish, so the tag users pull has passed exactly this:
#
#   1. It starts with no display and no locale warning, TLS included, and its
#      entry point, volumes, environment and source label are the documented ones.
#   2. The two tools it ships are there, and nothing graphical is linked.
#   3. ngPost.docker.conf, as shipped, is read without a single diagnostic.
#   4. A real post, as the wiki documents it: a folder packed by 7-Zip with a
#      password, par2cmdline recovery files, articles to an NNTP server that
#      checks the credentials, and an nzb that lists every one of them -- run as
#      an unprivileged user on a read-only root filesystem.
#
# Run it locally with DOCKER=podman; the NNTP server is the mock of the test
# suite, so python3 is needed on the host.
set -euo pipefail

image=${1:?usage: test-docker-image.sh <image>}
docker=${DOCKER:-docker}
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
mock_pid=
cleanup() {
    if [ -n "$mock_pid" ]; then
        kill "$mock_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "::error::$*"
    exit 1
}
step() { printf '\n== %s\n' "$*"; }

# The same uid inside and out: the files the container writes are the user's.
# Rootless podman needs its user namespace told so, SELinux relabelling off, and
# none of the tmpfs it adds to a read-only container on its own, which docker
# does not: the run must see the filesystem a docker user gets.
user=(--user "$(id -u):$(id -g)")
if [ "$(basename "$docker")" = podman ]; then
    user+=(--userns=keep-id --security-opt label=disable --read-only-tmpfs=false)
fi
# A diagnostic of ngPost, as opposed to the output a command asked for.
stamped='^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\]'

step "starts with no display, no locale warning, and TLS"
out=$("$docker" run --rm "$image" --version 2>&1)
echo "$out"
grep -q "SSL support: yes" <<<"$out" || fail "the image has no TLS support"
if grep -qi "locale" <<<"$out"; then
    fail "Qt complains about the locale"
fi
"$docker" run --rm "$image" >/dev/null || fail "the default command, --help, fails"

step "keeps its documented interface"
inspect() { "$docker" image inspect --format "$1" "$image"; }
[ "$(inspect '{{json .Config.Entrypoint}}')" = '["ngPost"]' ] || fail "the entry point is not ngPost"
env=$(inspect '{{range .Config.Env}}{{println .}}{{end}}')
grep -qx 'XDG_CONFIG_HOME=/config' <<<"$env" || fail "XDG_CONFIG_HOME is not /config"
grep -qx 'LANG=C.UTF-8' <<<"$env" || fail "LANG is not C.UTF-8"
# shellcheck disable=SC2016 # template variables, for docker inspect to expand
volumes=$(inspect '{{range $path, $v := .Config.Volumes}}{{println $path}}{{end}}')
for volume in /config /data; do
    grep -qx "$volume" <<<"$volumes" || fail "$volume is not a volume"
done
source=$(inspect '{{index .Config.Labels "org.opencontainers.image.source"}}')
[ "$source" = "https://github.com/Hydro74000/ngPost" ] \
    || fail "org.opencontainers.image.source is '$source': ghcr.io links the package to its repository with it"

step "ships its tools, and nothing graphical"
"$docker" run --rm --entrypoint sh "$image" -c '
    set -e
    command -v par2 >/dev/null || { echo "par2cmdline missing"; exit 1; }
    command -v 7z >/dev/null || { echo "7-Zip missing"; exit 1; }
    if ldd /usr/local/bin/ngPost | grep -E "libQt6Widgets|libQt6Gui|libX11|libGL"; then
        echo "the graphical stack is linked"
        exit 1
    fi
' || fail "the image does not hold what it promises"

mkdir -p "$work/config/ngPost" "$work/data/input/show" "$work/data/nzb" "$work/data/tmp"
chmod 700 "$work/config/ngPost"
run() {
    "$docker" run --rm "${user[@]}" --read-only --tmpfs /tmp \
        -v "$work/config:/config" -v "$work/data:/data" "$@"
}

step "reads the shipped configuration without a diagnostic"
cp "$repo/ngPost.docker.conf" "$work/config/ngPost/ngPost.conf"
chmod 600 "$work/config/ngPost/ngPost.conf"
out=$(run "$image" --history 2>&1)
echo "$out"
grep -q "Using default config file: /config/ngPost/ngPost.conf" <<<"$out" \
    || fail "the configuration was not read"
if grep -E "$stamped" <<<"$out"; then
    fail "ngPost.docker.conf is not read cleanly"
fi

step "posts a folder: 7-Zip, par2cmdline, NNTP, nzb"
python3 "$repo/tests/fixtures/mock_nntp/server.py" --port 0 --port-file "$work/port" \
    --dump-dir "$work/dump" --log-file "$work/mock.log" --require-auth myUser:myPass &
mock_pid=$!
for _ in $(seq 150); do
    [ -s "$work/port" ] && break
    sleep 0.1
done
[ -s "$work/port" ] || fail "the mock NNTP server did not start"
port=$(cat "$work/port")
# The shipped configuration, pointed at the mock server; the credentials are
# the placeholders it ships with, which the server checks.
sed -e 's/^host *=.*/host = 127.0.0.1/' -e "s/^port *=.*/port = $port/" \
    -e 's/^ssl *=.*/ssl = false/' -e 's/^connection *=.*/connection = 2/' \
    "$repo/ngPost.docker.conf" >"$work/config/ngPost/ngPost.conf"
head -c 3000000 /dev/urandom >"$work/data/input/show/episode.mkv"
head -c 700000 /dev/urandom >"$work/data/input/show/episode.nfo"
# The host network: the mock server listens on the runner's loopback.
out=$(run --network host "$image" -i /data/input/show -o /data/nzb/show.nzb \
    --compress --gen_par2 --gen_name --gen_pass 2>&1) || {
    echo "$out"
    cat "$work/mock.log" || true
    fail "the post failed"
}
echo "$out"

nzb="$work/data/nzb/show.nzb"
[ -f "$nzb" ] || fail "no nzb was written"
files=$(grep -c '<file ' "$nzb" || true)
segments=$(grep -c '<segment ' "$nzb" || true)
articles=$(find "$work/dump" -name '*.eml' | wc -l)
echo "nzb: $files files, $segments segments; server: $articles articles"
[ "$segments" -gt 0 ] && [ "$segments" -eq "$articles" ] \
    || fail "the nzb lists $segments segments, the server received $articles articles"
grep -q '\.7z' "$nzb" || fail "no 7-Zip volume in the nzb"
grep -q '\.par2' "$nzb" || fail "no par2 file in the nzb"
grep -q '<meta type="password">' "$nzb" || fail "the archive password is not in the nzb"
plain=$(find "$work/dump" -name '*.eml' -exec grep -L '=ybegin' {} + || true)
[ -z "$plain" ] || fail "articles not yEnc encoded: $plain"
leftovers=$(find "$work/data/tmp" -type f | wc -l)
[ "$leftovers" -eq 0 ] || fail "$leftovers temporary files left in /data/tmp"
if [ "$(find "$work/data" "$work/config" ! -user "$(id -u)" | wc -l)" -ne 0 ]; then
    fail "the container wrote files the user does not own"
fi

step "all good"
