#!/bin/bash
# ngpost-vpn-helper - privileged supervisor for the ngPost VPN tunnel.
#
# ngPost spawns us via a SINGLE pkexec call. We:
#  1. start the VPN (openvpn or wireguard-go)
#  2. wait for the tunnel to come up
#  3. install scoped policy routing so only ngPost's sockets exit via the VPN
#  4. signal readiness on stdout
#  5. wait for the parent to close stdin
#  6. cleanly tear everything down
#
# Communication with ngPost on stdout (one record per line):
#   READY <iface> <ip> [<dns_ip> | -]
#   ERROR <reason>
#   LOG <text>
# The parent closes our stdin to ask us to stop.
#
# Args:  $1 = openvpn|wireguard|cleanup    $2 = path to config file (n/a for cleanup)
#        --auth-file <path>   optional, after the config: forwarded to openvpn
#                             as `--auth-user-pass <path>` (no prompt mode).

set -u

BACKEND="${1:-}"
CONFIG="${2:-}"
shift 2 2>/dev/null || true

# Parse optional flags after backend + config.
AUTH_FILE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --auth-file)
            AUTH_FILE="${2:-}"
            shift 2
            ;;
        *)
            # Unknown flag — ignore (forward compat).
            shift
            ;;
    esac
done

TABLE=4242
PRIO=1042
LOG_FILE=$(mktemp /tmp/ngpost-vpn-XXXXXX.log)
# PID files let cleanup mode kill ONLY processes we know we started, instead
# of pkill-fing every openvpn on the box that happens to share an arg with us.
OVPN_PIDFILE=/run/ngpost-vpn-openvpn.pid
WG_IFACE=ngpost-wg0
# A short marker we drop on disk while running. Survives our death so the
# next ngPost startup can tell at a glance whether stale state may exist.
RUN_MARKER=/run/ngpost-vpn.running

VPN_PID=""
TUN_IFACE=""
TUN_IP=""
DNS_IP=""
WG_SETCONF_FILE=""

emit() { printf '%s\n' "$*"; }
log()  { emit "LOG $*"; }
fail() { emit "ERROR $*"; exit 1; }

cleanup() {
    if [ -n "$TUN_IP" ]; then
        ip rule del from "$TUN_IP" table "$TABLE" priority "$PRIO" 2>/dev/null || true
    fi
    ip route flush table "$TABLE" 2>/dev/null || true
    if [ -n "$VPN_PID" ] && kill -0 "$VPN_PID" 2>/dev/null; then
        kill -INT "$VPN_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$VPN_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$VPN_PID" 2>/dev/null || true
    fi
    # Kernel-mode WG has no userspace process: the interface itself must be
    # torn down explicitly. Safe to attempt unconditionally; failures (no
    # such iface, already removed) are ignored.
    if [ -n "$TUN_IFACE" ] && ip link show "$TUN_IFACE" >/dev/null 2>&1; then
        ip link del "$TUN_IFACE" 2>/dev/null || true
    fi
    # Drop the PID file too so the next startup's `cleanup` mode doesn't
    # think there's an orphan to clean.
    rm -f "$OVPN_PIDFILE" "$RUN_MARKER" "$LOG_FILE" "$WG_SETCONF_FILE"
}
trap cleanup EXIT TERM INT HUP

[ -n "$BACKEND" ] || fail "missing backend argument"
if [ "$BACKEND" != "cleanup" ]; then
    [ -n "$CONFIG" ] || fail "missing config argument"
    [ -r "$CONFIG" ] || fail "config not readable: $CONFIG"
fi

start_openvpn() {
    rm -f "$OVPN_PIDFILE"
    # Optional auth file (--auth-user-pass <file>): forwarded from ngPost when
    # the active profile has credentials in the keychain. openvpn-cli option
    # overrides any matching directive in the .ovpn.
    local extra_args=()
    if [ -n "$AUTH_FILE" ] && [ -r "$AUTH_FILE" ]; then
        extra_args+=(--auth-user-pass "$AUTH_FILE")
    fi

    openvpn \
        --config "$CONFIG" \
        --route-nopull \
        --script-security 0 \
        --pull-filter ignore redirect-gateway \
        --pull-filter ignore route \
        --management 127.0.0.1 7505 \
        --writepid "$OVPN_PIDFILE" \
        --verb 3 \
        "${extra_args[@]}" \
        > "$LOG_FILE" 2>&1 &
    VPN_PID=$!

    local i
    for i in $(seq 1 60); do
        kill -0 "$VPN_PID" 2>/dev/null || {
            tail -30 "$LOG_FILE" | sed 's/^/LOG /'
            fail "openvpn died before the tunnel was ready"
        }
        if grep -q "Initialization Sequence Completed" "$LOG_FILE"; then
            break
        fi
        sleep 0.5
    done

    TUN_IFACE=$(grep -oE 'TUN/TAP device [^ ]+ opened' "$LOG_FILE" \
                | head -1 | awk '{print $3}')
    # OpenVPN 2.6+ uses netlink and logs net_addr_v4_add: <ip>/<prefix> dev <iface>
    TUN_IP=$(grep -oE 'net_addr_v4_add: [0-9.]+(/[0-9]+)? dev [^ ]+' "$LOG_FILE" \
             | head -1 | awk '{print $2}' | cut -d/ -f1)
    if [ -z "$TUN_IP" ]; then
        # OpenVPN 2.4 fallback: /sbin/ip addr add dev <iface> local <ip>
        TUN_IP=$(grep -oE 'ip addr add dev [^ ]+ local [0-9.]+' "$LOG_FILE" \
                 | head -1 | awk '{print $6}')
    fi
    if [ -z "$TUN_IP" ]; then
        # /sbin/ifconfig <iface> <ip>
        TUN_IP=$(grep -oE 'ifconfig [^ ]+ [0-9.]+' "$LOG_FILE" \
                 | head -1 | awk '{print $3}')
    fi

    # Extract the DNS server pushed by the VPN (PUSH_REPLY ... dhcp-option DNS X.X.X.X).
    # The user opted into --pull-filter ignore redirect-gateway/route so we
    # don't touch the system, but the DNS push is just a string we parse here.
    DNS_IP=$(grep -oE "PUSH_REPLY[^\\n]*dhcp-option DNS [0-9.]+" "$LOG_FILE" \
             | head -1 | awk '{print $NF}')
}

write_wg_setconf_config() {
    WG_SETCONF_FILE=$(mktemp /tmp/ngpost-wg-setconf-XXXXXX.conf) \
        || fail "could not create temporary WireGuard config"

    # User-facing WireGuard profiles are commonly wg-quick configs. `wg setconf`
    # only accepts native WireGuard keys, so strip wg-quick-only [Interface]
    # options before configuring the interface. The original file remains the
    # source of truth for Address/DNS parsing below.
    awk '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s)
            sub(/[[:space:]]+$/, "", s)
            return s
        }

        /^[[:space:]]*\[/ {
            section = tolower(trim($0))
            print
            next
        }

        {
            raw = $0
            key = raw
            sub(/[[:space:]]*=.*/, "", key)
            key = tolower(trim(key))

            if (section == "[interface]" &&
                (key == "address" || key == "addresses" || key == "dns" ||
                 key == "mtu" || key == "table" || key == "preup" ||
                 key == "postup" || key == "predown" || key == "postdown" ||
                 key == "saveconfig")) {
                next
            }

            print raw
        }
    ' "$CONFIG" > "$WG_SETCONF_FILE" \
        || fail "could not prepare WireGuard setconf config"
}

start_wireguard() {
    TUN_IFACE="ngpost-wg0"

    # Prefer the in-kernel WireGuard implementation (Linux >= 5.6) — it's
    # what every modern distro ships, what the test environment uses for the
    # server side, and it removes a hard dependency on the userspace
    # `wireguard-go` binary which is not part of `wireguard-tools`.
    # Fall back to wireguard-go only when the kernel module is absent.
    if ip link add "$TUN_IFACE" type wireguard 2>>"$LOG_FILE"; then
        VPN_PID=""   # kernel iface, nothing to kill on cleanup beyond `ip link del`
    elif command -v wireguard-go >/dev/null 2>&1; then
        wireguard-go -f "$TUN_IFACE" > "$LOG_FILE" 2>&1 &
        VPN_PID=$!
        local i
        for i in $(seq 1 30); do
            [ -e "/sys/class/net/$TUN_IFACE" ] && break
            sleep 0.2
        done
        [ -e "/sys/class/net/$TUN_IFACE" ] || fail "wireguard-go did not create $TUN_IFACE"
    else
        fail "no WireGuard backend: kernel module unavailable AND wireguard-go not installed"
    fi

    write_wg_setconf_config
    wg setconf "$TUN_IFACE" "$WG_SETCONF_FILE" >>"$LOG_FILE" 2>&1 || {
        tail -30 "$LOG_FILE" | sed 's/^/LOG /'
        fail "wg setconf failed"
    }

    TUN_IP=$(sed -n '/^\[Interface\]/,/^\[/p' "$CONFIG" \
             | grep -E '^[[:space:]]*Address' \
             | head -1 | awk -F= '{print $2}' | tr -d ' ' | cut -d/ -f1 | cut -d, -f1)
    [ -n "$TUN_IP" ] || fail "no IPv4 Address= in [Interface] section of $CONFIG"

    # Optional DNS = X.X.X.X (first IP only) from the [Interface] section.
    DNS_IP=$(sed -n '/^\[Interface\]/,/^\[/p' "$CONFIG" \
             | grep -E '^[[:space:]]*DNS' \
             | head -1 | awk -F= '{print $2}' | tr -d ' ' | cut -d, -f1)

    ip addr add "$TUN_IP/32" dev "$TUN_IFACE" || fail "ip addr add failed"
    ip link set "$TUN_IFACE" up                || fail "ip link set up failed"
}

do_cleanup_only() {
    # Idempotent teardown of any stale state left by a previous run that died
    # without going through the normal trap (helper SIGKILL'd, OOM, host
    # reboot, ...). The whole point of this function is to be SAFE in the
    # presence of an unrelated VPN tunnel on the same machine. Each step
    # explicitly verifies we only touch processes / rules / routes that we
    # know belong to ngPost.
    #
    # We track whether anything was actually removed so the parent can tell
    # "no orphan, nothing to do" from "had to clean up after a previous
    # crash" — only the latter is interesting to log.
    local did_anything=0

    # --- openvpn: identify by pidfile, verify by cmdline signature ---
    if [ -r "$OVPN_PIDFILE" ]; then
        local ovpn_pid
        ovpn_pid=$(cat "$OVPN_PIDFILE" 2>/dev/null || true)
        if [ -n "$ovpn_pid" ] && [ -r "/proc/$ovpn_pid/cmdline" ]; then
            # Read the cmdline (null-separated args) and check our signature
            # exists. This guards against PID reuse / unrelated process.
            if tr '\0' ' ' < "/proc/$ovpn_pid/cmdline" \
                 | grep -q -- '--management 127.0.0.1 7505'; then
                kill -INT "$ovpn_pid" 2>/dev/null || true
                local i=0
                while kill -0 "$ovpn_pid" 2>/dev/null && [ "$i" -lt 10 ]; do
                    sleep 0.5; i=$((i+1))
                done
                kill -KILL "$ovpn_pid" 2>/dev/null || true
                did_anything=1
            fi
        fi
        rm -f "$OVPN_PIDFILE"
    fi

    # --- wireguard-go: identify by interface name (unique to us) ---
    # Find PIDs of "wireguard-go ... ngpost-wg0" exactly — the interface name
    # we use is unique to ngPost.
    for pid in $(pgrep -fa "wireguard-go" | awk -v wgif="$WG_IFACE" \
                       '$0 ~ ("[ ]"wgif"($|[ ])"){print $1}'); do
        [ -n "$pid" ] || continue
        # Final guard: verify the cmdline really ends with our iface name.
        if tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null \
             | grep -qE "wireguard-go[ -].*[ ]$WG_IFACE($|[ ])"; then
            kill -INT "$pid" 2>/dev/null || true
            did_anything=1
        fi
    done

    # --- ip rules: only drop those that match BOTH our priority AND
    # our table number. A foreign tool using priority 1042 with a different
    # table is left alone. ---
    while read -r from_ip; do
        [ -n "$from_ip" ] || continue
        if ip rule del from "$from_ip" table "$TABLE" priority "$PRIO" 2>/dev/null; then
            did_anything=1
        fi
    done < <(ip rule list | awk -v p="${PRIO}:" -v t="$TABLE" '
                 $1 == p {
                     has_t = 0
                     for (i=2;i<=NF;i++) if ($i=="lookup" && $(i+1)==t) has_t = 1
                     if (!has_t) next
                     for (i=2;i<=NF;i++) if ($i=="from") print $(i+1)
                 }')

    # --- ip route table: flush ours only (specific table number) ---
    # If a foreign tool also uses table 4242 (very unlikely, range 4000+ is
    # cold), their routes go too. The table number is documented as ngPost's.
    if [ -n "$(ip route show table "$TABLE" 2>/dev/null)" ]; then
        ip route flush table "$TABLE" 2>/dev/null || true
        did_anything=1
    fi

    # --- wireguard interface ngpost-wg0: drop it (name is ours) ---
    if ip link show "$WG_IFACE" >/dev/null 2>&1; then
        ip link del "$WG_IFACE" 2>/dev/null || true
        did_anything=1
    fi

    rm -f "$RUN_MARKER"
    # Only signal CLEANED when we actually removed something. A stale-but-
    # cosmetic pidfile alone is not worth a user-facing log line.
    if [ "$did_anything" -eq 1 ]; then
        echo "CLEANED"
    else
        echo "NOTHING_TO_CLEAN"
    fi
}

case "$BACKEND" in
    openvpn)   start_openvpn   ;;
    wireguard) start_wireguard ;;
    cleanup)   do_cleanup_only; trap - EXIT TERM INT HUP; rm -f "$LOG_FILE"; exit 0 ;;
    *)         fail "unknown backend: $BACKEND" ;;
esac

[ -n "$TUN_IFACE" ] || fail "could not determine tun interface"
[ -n "$TUN_IP"    ] || fail "could not determine tun IP"

# Best-effort cleanup of any stale state.
ip rule  del from "$TUN_IP" table "$TABLE" priority "$PRIO" 2>/dev/null || true
ip route flush table "$TABLE" 2>/dev/null || true

# Scoped policy routing: only packets whose source IP is $TUN_IP use $TABLE.
# Other apps stay on the system default route.
ip route add default dev "$TUN_IFACE" table "$TABLE" \
    || fail "ip route add default failed"
ip rule add from "$TUN_IP" table "$TABLE" priority "$PRIO" \
    || fail "ip rule add failed"

emit "READY $TUN_IFACE $TUN_IP ${DNS_IP:--}"

# Block until parent closes stdin (or sends 'stop').
while IFS= read -r line; do
    case "$line" in
        stop) break ;;
        *)    ;;
    esac
done

exit 0
