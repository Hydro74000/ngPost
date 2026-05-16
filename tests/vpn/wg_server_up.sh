#!/bin/bash
#
# wg_server_up.sh — bring up a self-hosted WireGuard server side for the
# Phase 5 E2E tests.
#
# The script generates a fresh keypair for the server AND for the client
# (ngPost), writes both configs into a state directory, and brings the
# server interface up. The mock NNTP server then listens on the server's
# tunnel IP so ngPost — when configured with the client conf and
# connected through the tunnel — reaches it via the tunnel, NOT loopback.
#
# Requires:
#   - wg-quick + wg + iproute2 + iptables installed
#   - passwordless sudo (or run as root)
#   - Linux kernel with the wireguard module (>= 5.6 mainline)
#
# Outputs (in $STATE_DIR):
#   server.conf        WireGuard server config (consumed by wg-quick)
#   client.conf        WireGuard client config (consumed by ngPost via VpnProfile)
#   server.addr        Server tunnel IP (eg. 10.42.0.1)
#   client.addr        Client tunnel IP (eg. 10.42.0.2)
#   listen.port        UDP port the server is bound to (eg. 51820)
#
# Run:    sudo STATE_DIR=/tmp/ngpost-wg tests/vpn/wg_server_up.sh
# Teardown: sudo STATE_DIR=/tmp/ngpost-wg tests/vpn/wg_server_down.sh

set -euo pipefail

STATE_DIR="${STATE_DIR:-/tmp/ngpost-wg}"
SERVER_ADDR="${SERVER_ADDR:-10.42.0.1/24}"
CLIENT_ADDR="${CLIENT_ADDR:-10.42.0.2/32}"
LISTEN_PORT="${LISTEN_PORT:-51820}"
IFACE="${IFACE:-ngpost-wg}"
UPSTREAM_IF="${UPSTREAM_IF:-}"

if [ "$EUID" -ne 0 ]; then
    echo "wg_server_up: must run as root (sudo)" >&2
    exit 1
fi

if ! command -v wg-quick >/dev/null; then
    echo "wg_server_up: wg-quick not installed (apt: wireguard-tools)" >&2
    exit 2
fi

umask 077
mkdir -p "$STATE_DIR"

# Generate keys (idempotent: skip if files already exist, so a re-run
# without down doesn't churn the public keys).
if [ ! -f "$STATE_DIR/server.key" ]; then
    wg genkey | tee "$STATE_DIR/server.key" | wg pubkey > "$STATE_DIR/server.pub"
fi
if [ ! -f "$STATE_DIR/client.key" ]; then
    wg genkey | tee "$STATE_DIR/client.key" | wg pubkey > "$STATE_DIR/client.pub"
fi

SERVER_KEY="$(cat "$STATE_DIR/server.key")"
SERVER_PUB="$(cat "$STATE_DIR/server.pub")"
CLIENT_KEY="$(cat "$STATE_DIR/client.key")"
CLIENT_PUB="$(cat "$STATE_DIR/client.pub")"

# Strip CIDR from the server addr for the client's Endpoint, but write the
# CIDR form into the [Interface] block (wg-quick expects it that way).
SERVER_IP_BARE="${SERVER_ADDR%/*}"
CLIENT_IP_BARE="${CLIENT_ADDR%/*}"

cat > "$STATE_DIR/server.conf" <<EOF
[Interface]
Address = $SERVER_ADDR
ListenPort = $LISTEN_PORT
PrivateKey = $SERVER_KEY

[Peer]
PublicKey = $CLIENT_PUB
AllowedIPs = $CLIENT_ADDR
EOF

cat > "$STATE_DIR/client.conf" <<EOF
[Interface]
PrivateKey = $CLIENT_KEY
Address = $CLIENT_ADDR

[Peer]
PublicKey = $SERVER_PUB
Endpoint = 127.0.0.1:$LISTEN_PORT
AllowedIPs = $SERVER_IP_BARE/32
PersistentKeepalive = 15
EOF

echo "$SERVER_IP_BARE" > "$STATE_DIR/server.addr"
echo "$CLIENT_IP_BARE" > "$STATE_DIR/client.addr"
echo "$LISTEN_PORT"    > "$STATE_DIR/listen.port"

# wg-quick looks up the interface name from the conf filename, so make
# sure ours matches $IFACE. We also kill any pre-existing instance to
# tolerate flakey teardown across CI re-runs.
if ip link show "$IFACE" >/dev/null 2>&1; then
    wg-quick down "$STATE_DIR/server.conf" 2>/dev/null || true
fi

# wg-quick refuses paths whose basename != iface name. Copy/link so the
# basename matches.
cp "$STATE_DIR/server.conf" "$STATE_DIR/$IFACE.conf"
wg-quick up "$STATE_DIR/$IFACE.conf"

# The test binary runs unprivileged after this script returns. Keep server
# secrets root-only, but hand back the client config and metadata it must
# stage/read for ngPost.
if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
    chown "$SUDO_UID:$SUDO_GID" \
        "$STATE_DIR/client.conf" \
        "$STATE_DIR/server.addr" \
        "$STATE_DIR/client.addr" \
        "$STATE_DIR/listen.port"
fi

# Print a one-line summary so callers can grep this output.
echo "WG_SERVER_UP iface=$IFACE addr=$SERVER_IP_BARE client=$CLIENT_IP_BARE port=$LISTEN_PORT state_dir=$STATE_DIR"
