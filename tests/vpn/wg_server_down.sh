#!/bin/bash
# Tear down the WireGuard server interface brought up by wg_server_up.sh.
# Idempotent — does nothing if the interface is already gone.

set -u

STATE_DIR="${STATE_DIR:-/tmp/ngpost-wg}"
IFACE="${IFACE:-ngpost-wg}"

if [ "$EUID" -ne 0 ]; then
    echo "wg_server_down: must run as root (sudo)" >&2
    exit 1
fi

if [ -f "/etc/wireguard/$IFACE.conf" ] && ip link show "$IFACE" >/dev/null 2>&1; then
    wg-quick down "$IFACE" || true
fi
rm -f "/etc/wireguard/$IFACE.conf"
echo "WG_SERVER_DOWN iface=$IFACE state_dir=$STATE_DIR"
