#!/bin/bash
# ngPost VPN privileged supervisor, protocol v2.
# Linux only. The process owns a machine-wide flock for its complete lifetime;
# stdin EOF and an independent parent watchdog both trigger teardown.

set -u
export LC_ALL=C

# Read by new ngPost binaries before elevation. Keeping the declaration near
# the top lets them reject a v1 helper before that old helper can touch any
# interface, route or process.
readonly NGPOST_VPN_HELPER_PROTOCOL=2

TABLE=4242
PRIO=1042
WG_IFACE=ngpost-wg0
LOCK_FILE=/run/lock/ngpost-vpn.lock
RUNTIME_DIR=/run/ngpost-vpn
OWNER_FILE=$RUNTIME_DIR/owner-v2
LEGACY_PIDFILE=/run/ngpost-vpn-openvpn.pid
LEGACY_MARKER=/run/ngpost-vpn.running

ACTION=${1:-}
shift 2>/dev/null || true
CONFIG=""
case "$ACTION" in
    openvpn|wireguard)
        CONFIG=${1:-}
        shift 2>/dev/null || true
        ;;
esac

AUTH_FILE=""
AUTH_STDIN=0
BIN_DIR=""
PROTOCOL=1
OWNER_PID=0
OWNER_START=0
WAIT_MINUTES=0
NONBLOCKING=0
CONFIRMED=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --auth-file|--bin-dir|--protocol|--owner-pid|--owner-start|--wait-minutes)
            # `shift 2` with a single argument left shifts nothing and returns
            # non-zero, so $# never decreases and this root process spins on
            # 100% of a core forever. Refuse the invocation instead.
            if [ "$#" -lt 2 ]; then
                printf 'ERROR %s requires a value\n' "$1"
                exit 2
            fi
            case "$1" in
                --auth-file)    AUTH_FILE=$2 ;;
                --bin-dir)      BIN_DIR=$2 ;;
                --protocol)     PROTOCOL=$2 ;;
                --owner-pid)    OWNER_PID=$2 ;;
                --owner-start)  OWNER_START=$2 ;;
                --wait-minutes) WAIT_MINUTES=$2 ;;
            esac
            shift 2
            ;;
        --auth-stdin) AUTH_STDIN=1; shift ;;
        --nonblocking) NONBLOCKING=1; shift ;;
        --yes) CONFIRMED=1; shift ;;
        *) shift ;;
    esac
done

proc_start_time() {
    local pid=${1:-0} stat rest
    local -a fields
    [ -r "/proc/$pid/stat" ] || return 1
    stat=$(cat "/proc/$pid/stat") || return 1
    rest=${stat##*) }
    read -r -a fields <<< "$rest"
    [ "${#fields[@]}" -ge 20 ] || return 1
    printf '%s' "${fields[19]}"
}

process_matches() {
    local pid=${1:-0} start=${2:-0}
    [ "$pid" -gt 0 ] 2>/dev/null || return 1
    [ -r "/proc/$pid/stat" ] || return 1
    [ "$start" = 0 ] || [ "$(proc_start_time "$pid" 2>/dev/null || echo x)" = "$start" ]
}

emit() { printf '%s\n' "$*"; }

percent_encode() {
    local input=${1:-} output="" char hex i
    for ((i=0; i<${#input}; ++i)); do
        char=${input:i:1}
        case "$char" in
            [a-zA-Z0-9._~:/@+-]) output+=$char ;;
            *) printf -v hex '%02X' "'$char"; output+="%$hex" ;;
        esac
    done
    printf '%s' "$output"
}

percent_decode() {
    local value=${1:-}
    printf '%b' "${value//%/\\x}"
}

v2_emit() { [ "$PROTOCOL" = 2 ] && emit "$*"; }

log() {
    if [ "$PROTOCOL" = 2 ]; then
        emit "LOG level=info detail=$(percent_encode "$*")"
    else
        emit "LOG $*"
    fi
}

terminal_error() {
    local failure=${1:-internal}; shift || true
    if [ "$PROTOCOL" = 2 ]; then
        emit "ERROR failure=$failure detail=$(percent_encode "$*")"
    else
        emit "ERROR $*"
    fi
}

case "$PROTOCOL" in
    1|"$NGPOST_VPN_HELPER_PROTOCOL") ;;
    *) emit "ERROR invalid helper protocol"; exit 1 ;;
esac
[ "$PROTOCOL" != "$NGPOST_VPN_HELPER_PROTOCOL" ] \
    || emit "PROTOCOL $NGPOST_VPN_HELPER_PROTOCOL"

if ! [[ "$OWNER_PID" =~ ^[0-9]+$ ]] \
   || ! [[ "$OWNER_START" =~ ^[0-9]+$ ]] \
   || ! [[ "$WAIT_MINUTES" =~ ^[0-9]+$ ]]; then
    terminal_error configuration "owner and lease-wait arguments must be unsigned integers"
    exit 1
fi
if [ "$WAIT_MINUTES" -gt 1440 ]; then
    terminal_error configuration "lease wait must be in 0..1440 minutes"
    exit 1
fi

case "$ACTION" in
    openvpn|wireguard|cleanup|cleanup-stale|cleanup-unattributed) ;;
    *) terminal_error configuration "unknown backend or action: $ACTION"; exit 1 ;;
esac

if [ -n "$BIN_DIR" ]; then
    canonical_bin_dir=$(readlink -f -- "$BIN_DIR" 2>/dev/null || true)
    bin_dir_mode=$(stat -c %a -- "$canonical_bin_dir" 2>/dev/null || echo 0)
    if [ -z "$canonical_bin_dir" ] || [ ! -d "$canonical_bin_dir" ] \
       || [ "$(stat -c %u -- "$canonical_bin_dir" 2>/dev/null || echo -1)" != 0 ] \
       || ! [[ "$bin_dir_mode" =~ ^[0-7]+$ ]] \
       || [ $((8#$bin_dir_mode & 0022)) -ne 0 ]; then
        terminal_error configuration "bundled VPN binary directory is not root-owned and immutable"
        exit 1
    fi
    for bundled_tool in openvpn wireguard-go wg; do
        [ ! -e "$canonical_bin_dir/$bundled_tool" ] \
            || { [ -f "$canonical_bin_dir/$bundled_tool" ] \
                 && [ ! -L "$canonical_bin_dir/$bundled_tool" ] \
                 && [ "$(stat -c %u -- "$canonical_bin_dir/$bundled_tool" 2>/dev/null || echo -1)" = 0 ] \
                 && bundled_mode=$(stat -c %a -- "$canonical_bin_dir/$bundled_tool" 2>/dev/null) \
                 && [[ "$bundled_mode" =~ ^[0-7]+$ ]] \
                 && [ $((8#$bundled_mode & 0022)) -eq 0 ]; } \
            || { terminal_error configuration "bundled VPN executable is not root-owned and immutable: $bundled_tool"; exit 1; }
    done
    BIN_DIR=$canonical_bin_dir
fi

# Compatibility callers do not provide owner metadata. pkexec execs this
# helper as the direct child, so PPID remains the ngPost process to supervise.
if [ "$OWNER_PID" -eq 0 ] && [ "$PPID" -gt 1 ]; then
    OWNER_PID=$PPID
    OWNER_START=$(proc_start_time "$OWNER_PID" 2>/dev/null || echo 0)
fi

runtime_failure() {
    local keyword=$1 path=$2 detail=$3 extra=${4:-}
    if [ "$PROTOCOL" = 2 ]; then
        emit "$keyword path=$(percent_encode "$path")${extra:+ $extra} detail=$(percent_encode "$detail")"
    else
        emit "ERROR $keyword: $detail"
    fi
    exit 1
}

runtime_type=$(findmnt -n -o FSTYPE --target /run 2>/dev/null || stat -f -c %T /run 2>/dev/null || true)
case "$runtime_type" in
    tmpfs|ramfs) ;;
    *) runtime_failure RUNTIME_NOT_VOLATILE /run "VPN runtime state must live on tmpfs or ramfs" "fstype=$(percent_encode "${runtime_type:-unknown}")" ;;
esac

[ ! -L /run/lock ] \
    || runtime_failure LEASE_UNAVAILABLE /run/lock "the lock directory must not be a symbolic link"
[ ! -L "$RUNTIME_DIR" ] \
    || runtime_failure LEASE_UNAVAILABLE "$RUNTIME_DIR" "the runtime directory must not be a symbolic link"
mkdir -p /run/lock 2>/dev/null \
    || runtime_failure LEASE_UNAVAILABLE /run/lock "cannot create the lock directory"
mkdir -p "$RUNTIME_DIR" 2>/dev/null \
    || runtime_failure LEASE_UNAVAILABLE "$RUNTIME_DIR" "cannot create the runtime directory"
lock_state_type=$(findmnt -n -o FSTYPE --target /run/lock 2>/dev/null \
                  || stat -f -c %T /run/lock 2>/dev/null || true)
case "$lock_state_type" in
    tmpfs|ramfs) ;;
    *) runtime_failure RUNTIME_NOT_VOLATILE /run/lock \
           "VPN lease state must remain on volatile storage" \
           "fstype=$(percent_encode "${lock_state_type:-unknown}")" ;;
esac
runtime_state_type=$(findmnt -n -o FSTYPE --target "$RUNTIME_DIR" 2>/dev/null \
                     || stat -f -c %T "$RUNTIME_DIR" 2>/dev/null || true)
case "$runtime_state_type" in
    tmpfs|ramfs) ;;
    *) runtime_failure RUNTIME_NOT_VOLATILE "$RUNTIME_DIR" \
           "VPN session state must remain on volatile storage" \
           "fstype=$(percent_encode "${runtime_state_type:-unknown}")" ;;
esac
[ "$(stat -c %u "$RUNTIME_DIR" 2>/dev/null || echo -1)" = 0 ] \
    || runtime_failure LEASE_UNAVAILABLE "$RUNTIME_DIR" "the runtime directory is not owned by root"
chmod 0755 "$RUNTIME_DIR" 2>/dev/null \
    || runtime_failure LEASE_UNAVAILABLE "$RUNTIME_DIR" "cannot secure the runtime directory"

command -v flock >/dev/null 2>&1 \
    || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "flock is not installed"
[ ! -L "$LOCK_FILE" ] \
    || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "the VPN lease must not be a symbolic link"
[ ! -e "$LOCK_FILE" ] || [ -f "$LOCK_FILE" ] \
    || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "the VPN lease is not a regular file"
exec {LEASE_FD}<>"$LOCK_FILE" \
    || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "cannot open the VPN lease"

valid_lock_record() {
    local record=${1:-}
    [ "${#record}" -lt 512 ] || return 1
    [[ "$record" =~ ^v=1\ owner_pid=[0-9]+\ owner_start=[0-9]+\ owner_uid=[0-9]+\ helper_pid=[0-9]+\ backend=[a-z-]+\ since=[0-9]+\ end=1$ ]]
}

field_from_record() {
    local record=${1:-} key=$2 token
    for token in $record; do
        case "$token" in "$key"=*) printf '%s' "${token#*=}"; return 0 ;; esac
    done
    return 1
}

read_lock_owner() {
    local record
    record=$(head -c 512 "$LOCK_FILE" 2>/dev/null || true)
    valid_lock_record "$record" || return 1
    printf '%s' "$record"
}

emit_owner_state() {
    local keyword=$1 deadline=${2:-0} record owner helper uid backend since now age
    record=$(read_lock_owner || true)
    if [ -n "$record" ]; then
        owner=$(field_from_record "$record" owner_pid || echo 0)
        helper=$(field_from_record "$record" helper_pid || echo 0)
        uid=$(field_from_record "$record" owner_uid || echo 0)
        backend=$(field_from_record "$record" backend || echo unknown)
        since=$(field_from_record "$record" since || echo 0)
    else
        owner=0; helper=0; uid=0; backend=unknown; since=0
    fi
    now=$(date +%s)
    age=$(( now > since ? now - since : 0 ))
    if [ "$keyword" = WAITING ] && [ "$PROTOCOL" = 2 ]; then
        emit "WAITING owner_pid=$owner helper_pid=$helper owner_uid=$uid backend=$backend since=$since age=$age deadline=$deadline"
    elif [ "$keyword" = WAITING ]; then
        log "VPN lease busy; waiting for owner pid $owner (helper pid $helper)"
    elif [ "$PROTOCOL" = 2 ]; then
        emit "BUSY owner_pid=$owner helper_pid=$helper owner_uid=$uid backend=$backend since=$since age=$age"
    else
        emit "ERROR VPN lease busy (owner pid $owner, helper pid $helper)"
    fi
}

acquire_lease() {
    if flock -n "$LEASE_FD"; then return 0; fi
    if [ "$NONBLOCKING" -eq 1 ] || [ "$WAIT_MINUTES" -eq 0 ]; then
        emit_owner_state BUSY
        return 1
    fi
    local seconds deadline now record last_record
    seconds=$((WAIT_MINUTES * 60))
    deadline=$(( $(date +%s) + seconds ))
    last_record=$(read_lock_owner || true)
    emit_owner_state WAITING "$deadline"
    while true; do
        if [ "$OWNER_PID" -gt 0 ] 2>/dev/null \
           && ! process_matches "$OWNER_PID" "$OWNER_START"; then
            terminal_error helper_exited "ngPost parent exited while waiting for the VPN lease"
            return 1
        fi
        flock -n "$LEASE_FD" && return 0
        now=$(date +%s)
        [ "$now" -lt "$deadline" ] || break
        record=$(read_lock_owner || true)
        if [ "$record" != "$last_record" ]; then
            last_record=$record
            emit_owner_state WAITING "$deadline"
        fi
        sleep 1
    done
    local owner helper
    record=$(read_lock_owner || true)
    owner=$(field_from_record "$record" owner_pid 2>/dev/null || echo 0)
    helper=$(field_from_record "$record" helper_pid 2>/dev/null || echo 0)
    if [ "$PROTOCOL" = 2 ]; then
        emit "LEASE_TIMEOUT owner_pid=$owner helper_pid=$helper waited_seconds=$seconds"
    else
        emit "ERROR VPN lease wait timed out"
    fi
    return 1
}

acquire_lease || exit 2

lease_fd_identity=$(stat -L -c '%d:%i' "/proc/self/fd/$LEASE_FD" 2>/dev/null || true)
lease_path_identity=$(stat -L -c '%d:%i' "$LOCK_FILE" 2>/dev/null || true)
# Written as an explicit negated if rather than `A && B || C`: in that form the
# trailing branch also runs when an earlier test succeeded but a later one
# failed, which reads as if-then-else while behaving differently.
if ! { [ -n "$lease_fd_identity" ] && [ "$lease_fd_identity" = "$lease_path_identity" ] \
       && [ ! -L "$LOCK_FILE" ] \
       && [ "$(stat -c %u "$LOCK_FILE" 2>/dev/null || echo -1)" = 0 ]; }; then
    runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "the VPN lease changed or is not owned by root"
fi
chmod 0644 "$LOCK_FILE" 2>/dev/null \
    || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "cannot secure the VPN lease"

write_lock_record() {
    local backend=${1:-unknown} record expected actual
    record="v=1 owner_pid=$OWNER_PID owner_start=$OWNER_START owner_uid=${PKEXEC_UID:-${SUDO_UID:-$(id -u)}} helper_pid=$$ backend=$backend since=$(date +%s) end=1"
    valid_lock_record "$record" || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "invalid lock metadata"
    # The lease descriptor has just been opened, so its offset is zero. Truncate
    # the locked inode through /proc, then publish the sub-512-byte record with
    # one shell-builtin write and validate the exact resulting length. This
    # avoids adding Python/Perl as a privileged runtime dependency.
    if ! truncate -s 0 "/proc/self/fd/$LEASE_FD" 2>/dev/null \
       || ! printf '%s' "$record" >&"$LEASE_FD"; then
        runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "short or failed lock metadata write"
    fi
    expected=${#record}
    actual=$(stat -L -c %s "/proc/self/fd/$LEASE_FD" 2>/dev/null || echo -1)
    [ "$actual" = "$expected" ] \
        || runtime_failure LEASE_UNAVAILABLE "$LOCK_FILE" "short or failed lock metadata write"
}
write_lock_record "$ACTION"

SESSION_ID="${OWNER_PID:-0}-$(date +%s)-$$-$RANDOM"
SESSION_DIR=$RUNTIME_DIR/session-$SESSION_ID
PRIVATE_DIR=$SESSION_DIR/private
# Files the profile refers to are staged here and nowhere else. Keeping them
# out of PRIVATE_DIR is what makes a collision with the helper's own session
# files impossible rather than merely unlisted.
SESSION_PROFILE_DIR=$PRIVATE_DIR/profile
SESSION_CONFIG=$PRIVATE_DIR/config
SESSION_AUTH=$PRIVATE_DIR/auth
LOG_FILE=$PRIVATE_DIR/backend.log
MGMT_PASS_FILE=$PRIVATE_DIR/management.password
WG_SETCONF_FILE=$PRIVATE_DIR/wg-setconf.conf
VPN_PID=""
VPN_START=0
VPN_EXE=""
TUN_IFACE=""
TUN_IP=""
DNS_IP=""
CONFIG_SHA=""
ROUTE_STATE=0
RULE_STATE=0
IFACE_STATE=0
WATCHDOG_PID=""
MGMT_FD=""
MGMT_PORT=0
ACTIVE=0
HEALTH_STATE=unknown
RECONNECT_SINCE=0
LAST_ERROR=""
# WG_SUSPECT_SINCE holds the instant a peer was first seen emitting, not the
# entry into Suspect: it is the origin the 130/180 s thresholds are measured
# from for a peer that has no handshake yet, and the base of the 25 s
# confirmation when traffic resumes on an already stale handshake.
declare -A WG_PREV_TX WG_EMITTED WG_SUSPECT_SINCE WG_STATE
STRUCTURAL_FAILURES=0
OWNS_MANIFEST=0

manifest_value() {
    local record=$1 key=$2 token
    for token in $record; do
        case "$token" in "$key"=*) printf '%s' "${token#*=}"; return 0 ;; esac
    done
    return 1
}

publish_manifest() {
    local phase=$1 temp record
    temp=$(mktemp "$RUNTIME_DIR/.owner-v2.XXXXXX") || return 1
    record="v=2 owner_pid=$OWNER_PID owner_start=$OWNER_START helper_pid=$$ backend=$ACTION session=$SESSION_ID phase=$phase config=$(percent_encode "$SESSION_CONFIG") config_sha=$CONFIG_SHA vpn_pid=${VPN_PID:-0} vpn_start=${VPN_START:-0} exe=$(percent_encode "$VPN_EXE") iface=${TUN_IFACE:--} tun_ip=${TUN_IP:--} route=$ROUTE_STATE rule=$RULE_STATE interface=$IFACE_STATE end=1"
    if ! printf '%s\n' "$record" > "$temp" || ! chmod 0644 "$temp" || ! mv -f "$temp" "$OWNER_FILE"; then
        rm -f "$temp"
        return 1
    fi
    OWNS_MANIFEST=1
}

valid_manifest_record() {
    local record=${1:-}
    [ "${#record}" -lt 2048 ] || return 1
    # v2 writes a fixed-order, single-line record. Keeping the validator just
    # as strict prevents a truncated, duplicated or hand-crafted manifest
    # from authorising privileged cleanup.
    [[ "$record" =~ ^v=2\ owner_pid=[0-9]+\ owner_start=[0-9]+\ helper_pid=[0-9]+\ backend=(openvpn|wireguard)\ session=[a-zA-Z0-9_-]+\ phase=[a-z_]+\ config=[a-zA-Z0-9._~:/@+%-]*\ config_sha=([0-9a-f]{64})?\ vpn_pid=[0-9]+\ vpn_start=[0-9]+\ exe=[a-zA-Z0-9._~:/@+%-]*\ iface=([-a-zA-Z0-9_.]+)\ tun_ip=(-|[0-9.]+)\ route=(0|1|intent)\ rule=(0|1|intent)\ interface=(0|1|intent)\ end=1$ ]]
}

resolve_tool() {
    local name=$1
    if [ -x "/var/lib/ngpost/bin/$name" ]; then
        printf '%s' "/var/lib/ngpost/bin/$name"
    elif [ -n "$BIN_DIR" ] && [ -x "$BIN_DIR/$name" ]; then
        printf '%s' "$BIN_DIR/$name"
    else
        command -v "$name" 2>/dev/null || true
    fi
}

OPENVPN_BIN=$(resolve_tool openvpn)
WG_BIN=$(resolve_tool wg)
WIREGUARD_GO_BIN=$(resolve_tool wireguard-go)

exact_cmdline_pair() {
    local pid=$1 wanted_key=$2 wanted_value=$3 arg previous=""
    [ -r "/proc/$pid/cmdline" ] || return 1
    while IFS= read -r -d '' arg; do
        if [ "$previous" = "$wanted_key" ] && [ "$arg" = "$wanted_value" ]; then return 0; fi
        previous=$arg
    done < "/proc/$pid/cmdline"
    return 1
}

validated_openvpn() {
    local pid=$1 start=$2 exe=$3 config=$4 sha=$5 actual_exe
    process_matches "$pid" "$start" || return 1
    actual_exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    [ -n "$actual_exe" ] && [ "$actual_exe" = "$(readlink -f "$exe" 2>/dev/null || echo "$exe")" ] || return 1
    exact_cmdline_pair "$pid" --config "$config" || return 1
    [ -r "$config" ] || return 1
    [ "$(sha256sum "$config" | awk '{print $1}')" = "$sha" ] || return 1
}

validated_wireguard_go() {
    local pid=$1 start=$2 exe=$3 actual_exe
    process_matches "$pid" "$start" || return 1
    actual_exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    [ -n "$actual_exe" ] \
        && [ "$actual_exe" = "$(readlink -f "$exe" 2>/dev/null || echo "$exe")" ] \
        || return 1
    tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | grep -Fxq "$WG_IFACE"
}

terminate_pid() {
    local pid=$1 i
    kill -TERM "$pid" 2>/dev/null || true
    for i in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.5
    done
    kill -KILL "$pid" 2>/dev/null || true
}

cleanup_resources() {
    trap - TERM INT HUP
    [ -z "$MGMT_FD" ] || eval "exec ${MGMT_FD}>&-" 2>/dev/null || true
    MGMT_FD=""
    if [ -n "$VPN_PID" ] && validated_openvpn "$VPN_PID" "$VPN_START" "$VPN_EXE" "$SESSION_CONFIG" "$CONFIG_SHA"; then
        terminate_pid "$VPN_PID"
    elif [ -n "$VPN_PID" ] \
         && validated_wireguard_go "$VPN_PID" "$VPN_START" "$VPN_EXE"; then
        terminate_pid "$VPN_PID"
    fi
    [ "$RULE_STATE" = 0 ] || ip rule del from "$TUN_IP" table "$TABLE" priority "$PRIO" 2>/dev/null || true
    [ "$ROUTE_STATE" = 0 ] || ip route flush table "$TABLE" 2>/dev/null || true
    if [ "$IFACE_STATE" != 0 ] && [ "$TUN_IFACE" = "$WG_IFACE" ]; then ip link del "$WG_IFACE" 2>/dev/null || true; fi
    VPN_PID=""; VPN_START=0; VPN_EXE=""; TUN_IFACE=""; TUN_IP=""; DNS_IP=""
    ROUTE_STATE=0; RULE_STATE=0; IFACE_STATE=0; RECONNECT_SINCE=0
    trap 'exit 0' TERM INT HUP
}

# Invoked by `trap cleanup_all EXIT` below, which ShellCheck before 0.10 does
# not follow: it reports the function as never called (SC2329) and then every
# command in its body as unreachable (SC2317). Both are false here -- the trap
# is the only thing that runs this -- so the invocation is declared rather than
# the findings silenced wholesale; nothing else in the file is exempted.
# shellcheck disable=SC2329,SC2317
cleanup_all() {
    local code=$?
    [ -z "$WATCHDOG_PID" ] || kill "$WATCHDOG_PID" 2>/dev/null || true
    cleanup_resources
    [ "$OWNS_MANIFEST" -eq 0 ] || rm -f "$OWNER_FILE"
    rm -rf "$SESSION_DIR"
    exit "$code"
}
trap cleanup_all EXIT
trap 'exit 0' TERM INT HUP

cleanup_manifest_record() {
    local record=$1 backend pid start exe config sha iface ip route rule interface old_session
    valid_manifest_record "$record" || return 1
    backend=$(manifest_value "$record" backend || true)
    pid=$(manifest_value "$record" vpn_pid || echo 0)
    start=$(manifest_value "$record" vpn_start || echo 0)
    exe=$(percent_decode "$(manifest_value "$record" exe || true)")
    config=$(percent_decode "$(manifest_value "$record" config || true)")
    sha=$(manifest_value "$record" config_sha || true)
    iface=$(manifest_value "$record" iface || true)
    ip=$(manifest_value "$record" tun_ip || true)
    route=$(manifest_value "$record" route || echo 0)
    rule=$(manifest_value "$record" rule || echo 0)
    interface=$(manifest_value "$record" interface || echo 0)
    if [ "$backend" = openvpn ] && [ "$pid" -gt 0 ] 2>/dev/null \
       && validated_openvpn "$pid" "$start" "$exe" "$config" "$sha"; then
        terminate_pid "$pid"
    elif [ "$backend" = wireguard ] && [ "$pid" -gt 0 ] 2>/dev/null \
         && validated_wireguard_go "$pid" "$start" "$exe"; then
        terminate_pid "$pid"
    fi
    [ "$rule" = 0 ] || [ -z "$ip" ] || [ "$ip" = - ] || ip rule del from "$ip" table "$TABLE" priority "$PRIO" 2>/dev/null || true
    [ "$route" = 0 ] || ip route flush table "$TABLE" 2>/dev/null || true
    if [ "$interface" != 0 ] && [ "$iface" = "$WG_IFACE" ]; then ip link del "$WG_IFACE" 2>/dev/null || true; fi
    old_session=$(manifest_value "$record" session || true)
    case "$old_session" in *[!a-zA-Z0-9_-]*|'') ;; *) rm -rf "$RUNTIME_DIR/session-$old_session" ;; esac
    rm -f "$OWNER_FILE"
}

has_unattributed_artifacts() {
    ip link show "$WG_IFACE" >/dev/null 2>&1 && return 0
    ip rule show priority "$PRIO" 2>/dev/null | grep -q "lookup $TABLE" && return 0
    [ -n "$(ip route show table "$TABLE" 2>/dev/null)" ] && return 0
    [ -e "$LEGACY_PIDFILE" ] || [ -e "$LEGACY_MARKER" ]
}

find_legacy_helper() {
    local cmd pid arg previous=""
    for cmd in /proc/[0-9]*/cmdline; do
        [ -r "$cmd" ] || continue
        pid=${cmd#/proc/}; pid=${pid%/cmdline}
        previous=""
        while IFS= read -r -d '' arg; do
            if [[ "${previous##*/}" = ngpost-vpn-helper.sh ]] \
               && { [ "$arg" = openvpn ] || [ "$arg" = wireguard ]; }; then
                printf '%s' "$pid"
                return 0
            fi
            previous=$arg
        done < "$cmd"
    done
    return 1
}

if [ "$ACTION" = cleanup-stale ] || [ "$ACTION" = cleanup ]; then
    if [ -r "$OWNER_FILE" ]; then
        old_record=$(head -c 2048 "$OWNER_FILE")
        cleanup_manifest_record "$old_record" || { v2_emit "UNATTRIBUTED_VPN_STATE resources=malformed_manifest"; exit 3; }
        emit "CLEANED"
    elif has_unattributed_artifacts; then
        v2_emit "UNATTRIBUTED_VPN_STATE resources=$(percent_encode "interface/rule/route or legacy marker")"
        [ "$PROTOCOL" = 2 ] || emit "ERROR unattributed VPN state; not cleaned"
        exit 3
    else
        emit "NOTHING_TO_CLEAN"
    fi
    exit 0
fi

if [ "$ACTION" = cleanup-unattributed ]; then
    [ "$CONFIRMED" -eq 1 ] || { terminal_error configuration "explicit --yes is required"; exit 2; }
    legacy_helper=$(find_legacy_helper || true)
    if [ -n "$legacy_helper" ]; then
        v2_emit "LEGACY_OWNER_ACTIVE owner_pid=0 helper_pid=$legacy_helper resources=legacy_helper"
        exit 3
    fi
    if [ -r "$LEGACY_PIDFILE" ]; then
        legacy_pid=$(head -n 1 "$LEGACY_PIDFILE" 2>/dev/null || echo 0)
        if process_matches "$legacy_pid" 0 && tr '\0' '\n' < "/proc/$legacy_pid/cmdline" 2>/dev/null | grep -Fxq 7505; then
            v2_emit "LEGACY_OWNER_ACTIVE owner_pid=0 helper_pid=0 vpn_pid=$legacy_pid resources=openvpn"
            exit 3
        fi
    fi
    ip link del "$WG_IFACE" 2>/dev/null || true
    while read -r source; do
        [ -z "$source" ] || ip rule del from "$source" table "$TABLE" priority "$PRIO" 2>/dev/null || true
    done < <(ip rule show priority "$PRIO" 2>/dev/null | awk -v table="$TABLE" '$0 ~ ("lookup " table) {for(i=1;i<=NF;i++) if($i=="from") print $(i+1)}')
    ip route flush table "$TABLE" 2>/dev/null || true
    rm -f "$LEGACY_PIDFILE" "$LEGACY_MARKER"
    emit "CLEANED"
    exit 0
fi

case "$ACTION" in openvpn|wireguard) ;; *) terminal_error configuration "unknown backend: $ACTION"; exit 1 ;; esac
if [ -z "$CONFIG" ] || [ ! -r "$CONFIG" ]; then
    terminal_error configuration "config is missing or unreadable: $CONFIG"
    exit 1
fi

if [ -r "$OWNER_FILE" ]; then
    old_record=$(head -c 2048 "$OWNER_FILE")
    cleanup_manifest_record "$old_record" || { v2_emit "UNATTRIBUTED_VPN_STATE resources=malformed_manifest"; exit 3; }
elif has_unattributed_artifacts; then
    v2_emit "UNATTRIBUTED_VPN_STATE resources=$(percent_encode "interface/rule/route or legacy marker")"
    [ "$PROTOCOL" = 2 ] || emit "ERROR unattributed VPN state; not cleaned"
    exit 3
fi

# ---------------------------------------------------------------------------
# OpenVPN profile policy.
#
# This helper runs as root, and the Polkit rule lets the configured user reach
# it without a password. The profile was the way through that boundary: the
# `plugin` directive loads a shared library into the OpenVPN process, and it
# does so whatever --script-security says -- scripts and plugins are separate
# mechanisms, and only the first is governed by that option. Any process
# running as the user could write a profile naming a library it controls.
#
# So the profile is not copied any more: it is regenerated from directives on a
# whitelist, and anything absent from it is refused rather than passed through.
# ngPost checks the same lists before it ever calls pkexec, which is where the
# user gets a readable explanation; this copy is the one that defends the
# boundary, because the helper must not trust its caller.
#
# These four lists are kept identical to src/vpn/OpenVpnConfigPolicy.cpp by
# tests/unit/tst_OpenVpnConfigPolicy. Edit them together.
OPENVPN_ALLOWED_DIRECTIVES="
allow-compression auth auth-nocache auth-retry auth-user-pass
block-outside-dns cipher client comp-lzo compress connect-retry
connect-retry-max connect-timeout data-ciphers data-ciphers-fallback dev
dev-type dhcp-option disable-occ explicit-exit-notify fast-io float fragment
hand-window http-proxy http-proxy-retry http-proxy-timeout inactive keepalive
key-direction link-mtu lport mssfix mute mute-replay-warnings ncp-ciphers
ncp-disable nobind ns-cert-type opt-verify peer-fingerprint persist-key
persist-remote-ip persist-tun ping ping-exit ping-restart ping-timer-rem port
proto pull pull-filter rcvbuf remote remote-cert-eku remote-cert-ku
remote-cert-tls remote-random remote-random-hostname reneg-bytes reneg-pkts
reneg-sec resolv-retry route-nopull rport server-poll-timeout sndbuf
socket-flags socks-proxy socks-proxy-retry static-challenge
suppress-timestamps tls-cipher tls-ciphersuites tls-client tls-version-max
tls-version-min topology tran-window tun-mtu tun-mtu-extra verb
verify-x509-name
"

# Their argument names a file. Inline is preferred; a plain sibling name is
# accepted so a multi-file provider bundle still imports. Never a path: the
# process reading it is root, and "ca /etc/shadow" would be a read primitive.
OPENVPN_FILE_BEARING_DIRECTIVES="
ca cert crl-verify dh extra-certs key pkcs12 secret tls-auth tls-crypt
tls-crypt-v2
"

# Inline blocks whose body is an opaque blob -- PEM, a static key, a
# fingerprint list. `connection` is absent on purpose: its body is more
# directives, and it is validated line by line like the rest of the file.
OPENVPN_INLINE_BLOB_TAGS="
ca cert crl-verify dh extra-certs key peer-fingerprint pkcs12 secret tls-auth
tls-crypt tls-crypt-v2
"

# Recognised, accepted, and left out of the configuration this helper generates.
# They are routing statements: --route-nopull governs only what the SERVER
# pushes, so one written in the profile itself is applied regardless. ngPost
# does its own policy routing and has no use for them. Dropped rather than
# refused because redirect-gateway is in very nearly every provider profile,
# and --route-noexec on the command line below is the second, independent
# reason no route from a profile is ever installed.
OPENVPN_DROPPED_DIRECTIVES="
redirect-gateway redirect-private route route-delay route-metric
"

# Refused with a name of their own. Everything outside every list here is
# refused too; these are the ones a profile does not carry by accident.
OPENVPN_DENIED_DIRECTIVES="
askpass auth-user-pass-verify capath cd chroot client-config-dir
client-connect client-disconnect config daemon dev-node down down-pre engine
group http-proxy-user-pass ifconfig-pool-persist ipchange iproute
learn-address log log-append management management-client
management-client-auth management-client-pf management-external-cert
management-external-key management-hold management-query-passwords
management-query-proxy management-query-remote management-signal
management-up-down pkcs11-providers plugin providers route-pre-down route-up
script-security setcon setenv setenv-safe status status-version
tls-export-cert tls-verify tmp-dir up up-restart user writepid
"

#! Copy the files listed in "$1.files" -- written by sanitize_openvpn_profile as
#! it validated them -- out of the caller's directory $2 and into the private
#! staging directory, as root-owned 0600 copies. OpenVPN is then run with --cd
#! there, so it never opens a path the caller can still change.
#!
#! The list comes from the validating pass and is never re-derived here: a
#! second parser disagreeing about where an inline block ends turned PEM body
#! text back into directives, and "key ../config" then overwrote the sanitized
#! profile itself with the caller's raw file. Each name is checked again below
#! anyway, so a future drift confines the damage to nothing rather than to a
#! root write outside the session.
#!
#! Symbolic links are refused rather than followed: the caller owns that
#! directory, so "ca ca.crt" could be a link to a file only root can read --
#! and a startup failure hands the last lines of OpenVPN's log back to the
#! caller, which makes that a read-back channel rather than a silent one. The
#! window between that test and the copy is a race this cannot close from
#! shell; closing it properly needs an O_NOFOLLOW open.
stage_openvpn_sibling_files() {
    local profile=$1 srcdir=$2 refs="$1.files" name src
    mkdir -p "$SESSION_PROFILE_DIR" || {
        terminal_error internal "cannot create the profile staging directory"
        return 1
    }
    chmod 0700 "$SESSION_PROFILE_DIR" || {
        terminal_error internal "cannot secure the profile staging directory"
        return 1
    }
    [ -f "$refs" ] || return 0

    while IFS= read -r name; do
        [ -n "$name" ] || continue
        # Second gate, deliberately redundant with the sanitizer: a name is a
        # plain file name or it is not staged at all. Without this, one bad
        # character reaches a root cp on both sides of the copy.
        case "$name" in
            . | .. | *[!A-Za-z0-9._-]*)
                terminal_error configuration \
                    "the OpenVPN profile refers to a file whose name ngPost will not stage"
                return 1
                ;;
        esac
        src=$srcdir/$name
        if [ -L "$src" ] || [ ! -f "$src" ]; then
            terminal_error configuration \
                "the OpenVPN profile refers to a file that is not a regular file beside it"
            return 1
        fi
        cp -- "$src" "$SESSION_PROFILE_DIR/$name" || {
            terminal_error configuration "cannot stage a file the OpenVPN profile refers to"
            return 1
        }
        chmod 0600 "$SESSION_PROFILE_DIR/$name" || {
            terminal_error internal "cannot secure a staged OpenVPN file"
            return 1
        }
    done < <(sort -u -- "$refs")
    rm -f "$refs"
    return 0
}

#! Rewrite $1 in place, keeping only whitelisted directives. Non-zero, with the
#! offending directive named on stderr, when the profile must be refused.
sanitize_openvpn_profile() {
    local profile=$1 sanitized="$1.sanitized" refs="$1.files" reason=""
    : > "$refs" || { terminal_error internal "cannot record the profile's file list"; return 1; }

    # awk would silently truncate at a NUL, so a profile holding one is refused
    # rather than half-read.
    if [ "$(wc -c < "$profile")" != "$(tr -d '\000' < "$profile" | wc -c)" ]; then
        terminal_error configuration "the OpenVPN profile contains binary data"
        return 1
    fi
    if [ "$(wc -c < "$profile")" -gt 1048576 ]; then
        terminal_error configuration "the OpenVPN profile is larger than a profile ever is"
        return 1
    fi

    reason=$(awk \
        -v ALLOWED="$OPENVPN_ALLOWED_DIRECTIVES" \
        -v FILEB="$OPENVPN_FILE_BEARING_DIRECTIVES" \
        -v BLOBS="$OPENVPN_INLINE_BLOB_TAGS" \
        -v DENIED="$OPENVPN_DENIED_DIRECTIVES" \
        -v DROPPED="$OPENVPN_DROPPED_DIRECTIVES" '
        function label(name) {
            gsub(/[^A-Za-z0-9._-]/, "", name)
            if (name == "") name = "unprintable"
            return substr(name, 1, 32)
        }
        function fail(kind, name, line) {
            printf("%s %s line %d", kind, label(name), line) > "/dev/stderr"
            bad = 1
            exit 1
        }
        function fill(list, set,   n, i, parts) {
            n = split(list, parts, /[ \t\n]+/)
            for (i = 1; i <= n; ++i) if (parts[i] != "") set[parts[i]] = 1
        }
        function unquote(s) {
            gsub(/^["\047]|["\047]$/, "", s)
            return s
        }
        BEGIN {
            fill(ALLOWED, allowed); fill(FILEB, fileb)
            fill(BLOBS, blob);      fill(DENIED, denied)
            fill(DROPPED, dropped)
            open = ""; openline = 0; inconn = 0; bad = 0
        }
        {
            line = $0
            sub(/\r$/, "", line)
            gsub(/^[ \t]+|[ \t]+$/, "", line)

            if (open != "") {
                print line
                if (tolower(line) == "</" open ">") open = ""
                next
            }
            if (line == "" || line ~ /^#/ || line ~ /^;/) next

            if (substr(line, 1, 1) == "<") {
                if (substr(line, length(line), 1) != ">") fail("malformed-block", line, NR)
                tag = tolower(substr(line, 2, length(line) - 2))
                gsub(/^[ \t]+|[ \t]+$/, "", tag)
                closing = (substr(tag, 1, 1) == "/")
                name = closing ? substr(tag, 2) : tag
                if (closing) {
                    if (name == "connection" && inconn) { inconn = 0; print line; next }
                    fail("unopened-block", name, NR)
                }
                if (name == "connection") {
                    if (inconn) fail("nested-connection", name, NR)
                    inconn = 1; print line; next
                }
                if (!(name in blob)) fail("unknown-block", name, NR)
                open = name; openline = NR; print line; next
            }

            n = split(line, tok, /[ \t]+/)
            d = unquote(tok[1])
            while (d ~ /^--/) sub(/^--/, "", d)
            d = tolower(d)

            if (d in denied) fail("dangerous-directive", d, NR)
            if (d in dropped) next
            isfile = (d in fileb)
            if (!isfile && !(d in allowed)) fail("unreviewed-directive", d, NR)
            if (d == "auth-user-pass" && n > 1) fail("unsafe-argument", d, NR)
            # OpenVPN takes an authentication FILE as a positional argument to
            # its proxy directives and hands the content to the proxy the
            # profile named: the read and the way off the machine in one line.
            if (d == "http-proxy") {
                if (n < 3 || n > 5) fail("unsafe-argument", d, NR)
                if (n >= 4 && unquote(tok[4]) != "auto" \
                    && unquote(tok[4]) != "auto-nct") fail("unsafe-argument", d, NR)
                if (n == 5 && tolower(unquote(tok[5])) !~ /^(none|basic|ntlm|ntlm2)$/) \
                    fail("unsafe-argument", d, NR)
            }
            if (d == "socks-proxy" && n > 3) fail("unsafe-argument", d, NR)
            if (isfile) {
                if (n < 2) fail("unsafe-argument", d, NR)
                arg = unquote(tok[2])
                # Same rule as OpenVpnConfigPolicy::isPlainSiblingName(): a
                # plain name, no directory part, no traversal, and nothing a
                # parser downstream would read back as an option.
                if (arg !~ /^[A-Za-z0-9._-]+$/ || arg ~ /^-/ || arg == "." \
                    || arg == ".." || length(arg) > 128) fail("unsafe-argument", d, NR)
                # Named here, by the pass that just validated it: staging must
                # never re-parse the profile on its own, because a second state
                # machine disagreeing about where an inline block ends is what
                # turns blob content back into directives.
                print arg > "/dev/fd/3"
            }
            print line
        }
        END {
            if (bad) exit 1
            if (open != "") fail("unclosed-block", open, openline)
            if (inconn) fail("unclosed-connection", "connection", 0)
        }
    ' "$profile" 2>&1 >"$sanitized" 3>"$refs") || {
        rm -f "$sanitized" "$refs"
        terminal_error configuration "OpenVPN profile refused: ${reason:-unparsable}"
        return 1
    }

    mv -- "$sanitized" "$profile" || {
        rm -f "$sanitized" "$refs"
        terminal_error internal "cannot install the sanitized OpenVPN profile"
        return 1
    }
    chmod 0600 "$profile"
    return 0
}

mkdir -p "$PRIVATE_DIR" || { terminal_error internal "cannot create private runtime directory"; exit 1; }
chmod 0700 "$SESSION_DIR" "$PRIVATE_DIR" || { terminal_error internal "cannot secure private runtime directory"; exit 1; }
cp -- "$CONFIG" "$SESSION_CONFIG" || { terminal_error configuration "cannot copy VPN config into volatile session"; exit 1; }
chmod 0600 "$SESSION_CONFIG"
# The session copy is what OpenVPN is handed, so it is the copy that gets
# regenerated from the whitelist. Before the lease turns into a running root
# process, and before anything reads a directive out of it.
if [ "$ACTION" = openvpn ]; then
    sanitize_openvpn_profile "$SESSION_CONFIG" || exit 1
    stage_openvpn_sibling_files "$SESSION_CONFIG" "$(dirname "$CONFIG")" || exit 1
fi
if [ -n "$AUTH_FILE" ] && [ -r "$AUTH_FILE" ]; then
    cp -- "$AUTH_FILE" "$SESSION_AUTH" || { terminal_error authentication "cannot copy authentication file"; exit 1; }
    chmod 0600 "$SESSION_AUTH"
fi
if [ "$AUTH_STDIN" -eq 1 ]; then
    auth_command=""
    if ! IFS= read -r -t 10 auth_command \
       || [[ "$auth_command" != AUTH\ * ]] \
       || [ -z "${auth_command#AUTH }" ] \
       || ! printf '%s' "${auth_command#AUTH }" | base64 -d > "$SESSION_AUTH" 2>/dev/null; then
        terminal_error authentication "credential transfer from ngPost failed"
        exit 1
    fi
    chmod 0600 "$SESSION_AUTH" \
        || { terminal_error authentication "cannot secure authentication file"; exit 1; }
fi
CONFIG_SHA=$(sha256sum "$SESSION_CONFIG" | awk '{print $1}')
publish_manifest prepared || { terminal_error internal "cannot publish session manifest"; exit 1; }

select_management_port() {
    local candidate i
    for i in $(seq 1 100); do
        candidate=$((20000 + RANDOM % 30000))
        if ! ss -H -ltn "sport = :$candidate" 2>/dev/null | grep -q .; then printf '%s' "$candidate"; return 0; fi
    done
    return 1
}

start_openvpn() {
    [ -n "$OPENVPN_BIN" ] || { LAST_ERROR="openvpn not found"; return 1; }
    VPN_EXE=$(readlink -f "$OPENVPN_BIN" 2>/dev/null || printf '%s' "$OPENVPN_BIN")
    MGMT_PORT=$(select_management_port) || { LAST_ERROR="no loopback management port available"; return 1; }
    printf '%s\n' "$(cat /proc/sys/kernel/random/uuid 2>/dev/null || printf '%s-%s' "$RANDOM" "$RANDOM")" > "$MGMT_PASS_FILE" || return 1
    chmod 0600 "$MGMT_PASS_FILE"
    TUN_IFACE=""; TUN_IP=""; DNS_IP=""; VPN_PID=""; VPN_START=0
    publish_manifest openvpn_intent || { LAST_ERROR="cannot publish OpenVPN intent"; return 1; }
    local auth_args=()
    [ ! -r "$SESSION_AUTH" ] || auth_args=(--auth-user-pass "$SESSION_AUTH")
    (
        # A long-lived child must never inherit the lease. Otherwise a
        # SIGKILL of the supervisor leaves flock owned by OpenVPN forever.
        exec {LEASE_FD}>&-
        # --cd into the staging directory, not the caller's: every file the
        # profile refers to has been copied there, root-owned, so OpenVPN cannot
        # be pointed at a symbolic link to somebody else's file. It holds only
        # those copies, so a profile cannot name one of the helper's own session
        # files and have it replaced.
        # --route-noexec on top of --route-nopull: the latter only governs what
        # the SERVER pushes, while a route written in the profile itself is
        # applied regardless. The helper owns ngPost's routing; OpenVPN installs
        # none of its own.
        exec "$VPN_EXE" --cd "$SESSION_PROFILE_DIR" --config "$SESSION_CONFIG" \
            --route-nopull --route-noexec --script-security 0 \
            --pull-filter ignore redirect-gateway --pull-filter ignore route \
            --management 127.0.0.1 "$MGMT_PORT" "$MGMT_PASS_FILE" --management-signal \
            --verb 3 "${auth_args[@]}"
    ) > "$LOG_FILE" 2>&1 </dev/null &
    VPN_PID=$!
    VPN_START=$(proc_start_time "$VPN_PID" 2>/dev/null || echo 0)
    publish_manifest openvpn_started || { LAST_ERROR="cannot publish OpenVPN pid"; return 1; }
    local i
    for i in $(seq 1 60); do
        process_matches "$VPN_PID" "$VPN_START" || { LAST_ERROR="openvpn died before tunnel readiness"; return 1; }
        grep -q "Initialization Sequence Completed" "$LOG_FILE" && break
        sleep 0.5
    done
    grep -q "Initialization Sequence Completed" "$LOG_FILE" || { LAST_ERROR="openvpn readiness timeout"; return 1; }
    TUN_IFACE=$(grep -oE 'TUN/TAP device [^ ]+ opened' "$LOG_FILE" | head -1 | awk '{print $3}')
    [ -n "$TUN_IFACE" ] || TUN_IFACE=$(grep -E 'net_addr_v4_add: .* dev [^ ]+' "$LOG_FILE" | head -1 | awk '{for(i=1;i<NF;i++) if($i=="dev"){print $(i+1);exit}}')
    [ -n "$TUN_IFACE" ] || TUN_IFACE=$(ip -o -4 addr show | awk '$2 ~ /^(tun|tap|ovpn|dco)/ {print $2; exit}')
    [ -n "$TUN_IFACE" ] || { LAST_ERROR="cannot determine OpenVPN interface"; return 1; }
    TUN_IP=$(ip -o -4 addr show dev "$TUN_IFACE" 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}')
    [ -n "$TUN_IP" ] || { LAST_ERROR="cannot determine OpenVPN address"; return 1; }
    DNS_IP=$(grep -oE 'dhcp-option DNS [0-9.]+' "$LOG_FILE" | head -1 | awk '{print $NF}')
    publish_manifest tunnel_ready || return 1
    MGMT_FD=""
    for i in $(seq 1 25); do
        if exec {MGMT_FD}<>"/dev/tcp/127.0.0.1/$MGMT_PORT" 2>/dev/null; then
            break
        fi
        MGMT_FD=""
        sleep 0.1
    done
    if [ -n "$MGMT_FD" ]; then
        printf '%s\nstate on all\n' "$(head -n 1 "$MGMT_PASS_FILE")" >&"$MGMT_FD" || true
    else
        LAST_ERROR="OpenVPN management connection unavailable"
        return 1
    fi
}

prepare_wg_config() {
    awk '
      function trim(s){sub(/^[[:space:]]+/,"",s);sub(/[[:space:]]+$/,"",s);return s}
      /^[[:space:]]*\[/{section=tolower(trim($0));print;next}
      {raw=$0;key=raw;sub(/[[:space:]]*=.*/,"",key);key=tolower(trim(key));
       if(section=="[interface]" && (key=="address"||key=="addresses"||key=="dns"||key=="mtu"||key=="table"||key=="preup"||key=="postup"||key=="predown"||key=="postdown"||key=="saveconfig")) next;
       print raw}' "$SESSION_CONFIG" > "$WG_SETCONF_FILE"
}

start_wireguard() {
    [ -n "$WG_BIN" ] || { LAST_ERROR="wg not found"; return 1; }
    TUN_IFACE=$WG_IFACE; TUN_IP=""; DNS_IP=""; VPN_PID=""; VPN_START=0; VPN_EXE=""
    IFACE_STATE=intent
    publish_manifest interface_intent || return 1
    if ip link add "$WG_IFACE" type wireguard 2>>"$LOG_FILE"; then
        IFACE_STATE=1
    elif [ -n "$WIREGUARD_GO_BIN" ]; then
        VPN_EXE=$(readlink -f "$WIREGUARD_GO_BIN" 2>/dev/null || printf '%s' "$WIREGUARD_GO_BIN")
        (
            exec {LEASE_FD}>&-
            exec "$VPN_EXE" -f "$WG_IFACE"
        ) >> "$LOG_FILE" 2>&1 </dev/null &
        VPN_PID=$!; VPN_START=$(proc_start_time "$VPN_PID" 2>/dev/null || echo 0)
        local i
        for i in $(seq 1 30); do [ -e "/sys/class/net/$WG_IFACE" ] && break; sleep 0.2; done
        [ -e "/sys/class/net/$WG_IFACE" ] || { LAST_ERROR="wireguard-go did not create interface"; return 1; }
        IFACE_STATE=1
    else
        LAST_ERROR="kernel WireGuard and wireguard-go are unavailable"; return 1
    fi
    publish_manifest interface_created || return 1
    prepare_wg_config || { LAST_ERROR="cannot prepare WireGuard config"; return 1; }
    "$WG_BIN" setconf "$WG_IFACE" "$WG_SETCONF_FILE" >> "$LOG_FILE" 2>&1 || { LAST_ERROR="wg setconf failed"; return 1; }
    TUN_IP=$(sed -n '/^[[:space:]]*\[Interface\]/,/^[[:space:]]*\[/p' "$SESSION_CONFIG" | awk -F= 'tolower($1) ~ /^[[:space:]]*address/ {gsub(/[[:space:]]/,"",$2);split($2,a,"[,/]");print a[1];exit}')
    DNS_IP=$(sed -n '/^[[:space:]]*\[Interface\]/,/^[[:space:]]*\[/p' "$SESSION_CONFIG" | awk -F= 'tolower($1) ~ /^[[:space:]]*dns/ {gsub(/[[:space:]]/,"",$2);split($2,a,",");print a[1];exit}')
    [ -n "$TUN_IP" ] || { LAST_ERROR="no IPv4 Address in WireGuard config"; return 1; }
    ip addr add "$TUN_IP/32" dev "$WG_IFACE" || { LAST_ERROR="ip addr add failed"; return 1; }
    ip link set "$WG_IFACE" up || { LAST_ERROR="ip link set up failed"; return 1; }
    publish_manifest tunnel_ready || return 1
}

configure_policy_route() {
    ROUTE_STATE=intent; publish_manifest route_intent || return 1
    ip route add default dev "$TUN_IFACE" table "$TABLE" || { LAST_ERROR="policy route creation failed"; return 1; }
    ROUTE_STATE=1; publish_manifest route_created || return 1
    RULE_STATE=intent; publish_manifest rule_intent || return 1
    ip rule add from "$TUN_IP" table "$TABLE" priority "$PRIO" || { LAST_ERROR="policy rule creation failed"; return 1; }
    RULE_STATE=1; publish_manifest running || return 1
}

start_tunnel() {
    local attempt=${1:-0}
    LAST_ERROR=""
    WG_PREV_TX=(); WG_EMITTED=(); WG_SUSPECT_SINCE=(); WG_STATE=()
    STRUCTURAL_FAILURES=0
    if [ "$ACTION" = openvpn ]; then start_openvpn || return 1; else start_wireguard || return 1; fi
    configure_policy_route || return 1
    if [ "$PROTOCOL" = 2 ]; then
        emit "READY attempt_id=$attempt iface=$TUN_IFACE ip=$TUN_IP dns=${DNS_IP:--}"
    else
        emit "READY $TUN_IFACE $TUN_IP ${DNS_IP:--}"
    fi
    HEALTH_STATE=healthy
}

start_watchdog() {
    [ "$OWNER_PID" -gt 0 ] 2>/dev/null || return 0
    local supervisor_pid=$$ supervisor_start
    supervisor_start=$(proc_start_time "$supervisor_pid" 2>/dev/null || echo 0)
    (
        exec {LEASE_FD}>&-
        exec 0</dev/null
        exec 1>/dev/null 2>/dev/null
        while process_matches "$OWNER_PID" "$OWNER_START" \
              && process_matches "$supervisor_pid" "$supervisor_start"; do
            sleep 1
        done
        # Never signal a recycled helper PID. If the supervisor itself died,
        # the lease is already released and the next helper performs the
        # attributed cleanup from the manifest.
        if ! process_matches "$OWNER_PID" "$OWNER_START" \
           && process_matches "$supervisor_pid" "$supervisor_start"; then
            kill -TERM "$supervisor_pid" 2>/dev/null || true
        fi
    ) &
    WATCHDOG_PID=$!
}
start_watchdog

if ! start_tunnel 0; then
    tail -30 "$LOG_FILE" 2>/dev/null | while IFS= read -r line; do log "$line"; done
    terminal_error internal "${LAST_ERROR:-VPN startup failed}"
    exit 1
fi

set_global_health() {
    local next=$1 reason=${2:-}
    [ "$next" = "$HEALTH_STATE" ] && return
    HEALTH_STATE=$next
    case "$next" in
        healthy) v2_emit "HEALTHY backend=$ACTION" ;;
        suspect) v2_emit "SUSPECT backend=$ACTION reason=$(percent_encode "$reason")" ;;
        down) v2_emit "DOWN backend=$ACTION reason=$(percent_encode "$reason")" ;;
    esac
}

refresh_openvpn_identity() {
    local new_ip=$1 old_ip=$TUN_IP
    [ -n "$new_ip" ] && [[ "$new_ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] \
        || return 1
    [ "$new_ip" != "$old_ip" ] || return 0

    # OpenVPN can receive a different virtual address after a soft reconnect.
    # Replace the source rule transactionally and republish READY so ngPost
    # binds subsequent NNTP sockets to the new address.
    if [ "$RULE_STATE" != 0 ] && [ -n "$old_ip" ]; then
        ip rule del from "$old_ip" table "$TABLE" priority "$PRIO" 2>/dev/null || true
    fi
    RULE_STATE=0
    TUN_IP=$new_ip
    RULE_STATE=intent
    publish_manifest reconnect_rule_intent || return 1
    ip rule add from "$TUN_IP" table "$TABLE" priority "$PRIO" || return 1
    RULE_STATE=1
    publish_manifest running || return 1
    if [ "$PROTOCOL" = 2 ]; then
        emit "READY attempt_id=0 iface=$TUN_IFACE ip=$TUN_IP dns=${DNS_IP:--}"
    else
        emit "READY $TUN_IFACE $TUN_IP ${DNS_IP:--}"
    fi
}

poll_openvpn() {
    if ! process_matches "$VPN_PID" "$VPN_START"; then set_global_health down PROCESS_EXITED; return; fi
    local line state now connected_ip
    if [ -n "$MGMT_FD" ]; then
        while IFS= read -r -t 0.01 -u "$MGMT_FD" line; do
            case "$line" in
                \>STATE:*)
                    state=$(printf '%s' "${line#>STATE:}" | cut -d, -f2)
                    case "$state" in
                        CONNECTED)
                            connected_ip=$(printf '%s' "${line#>STATE:}" | cut -d, -f4)
                            if ! refresh_openvpn_identity "$connected_ip"; then
                                set_global_health down IDENTITY_UPDATE_FAILED
                                continue
                            fi
                            RECONNECT_SINCE=0
                            set_global_health healthy
                            ;;
                        RECONNECTING)
                            now=$(date +%s)
                            [ "$RECONNECT_SINCE" -ne 0 ] || RECONNECT_SINCE=$now
                            set_global_health suspect RECONNECTING
                            if [ $((now - RECONNECT_SINCE)) -ge 180 ]; then set_global_health down RECONNECT_TIMEOUT; fi
                            ;;
                    esac
                    ;;
            esac
        done
    fi
    now=$(date +%s)
    if [ "$RECONNECT_SINCE" -ne 0 ] \
       && [ $((now - RECONNECT_SINCE)) -ge 180 ]; then
        set_global_health down RECONNECT_TIMEOUT
    fi
}

poll_wireguard() {
    [ "$ACTIVE" -eq 1 ] || return
    local now dump peer handshake tx prev age suspect_since state
    local emitters=0 healthy=0 suspect=0 down=0
    now=$(date +%s)
    if ! ip link show "$WG_IFACE" >/dev/null 2>&1 \
       || ! ip -o -4 addr show dev "$WG_IFACE" | grep -q " $TUN_IP/" \
       || ! ip rule show priority "$PRIO" | grep -q "lookup $TABLE" \
       || [ -z "$(ip route show table "$TABLE" 2>/dev/null)" ]; then
        STRUCTURAL_FAILURES=$((STRUCTURAL_FAILURES + 1))
        [ "$STRUCTURAL_FAILURES" -lt 2 ] || set_global_health down STRUCTURE_MISSING
        return
    fi
    STRUCTURAL_FAILURES=0
    dump=$($WG_BIN show "$WG_IFACE" dump 2>/dev/null) || { set_global_health suspect WG_QUERY_FAILED; return; }
    while IFS=$'\t' read -r peer _ _ _ handshake _ tx _; do
        # The first line of `wg show <iface> dump` describes the interface and
        # carries only four fields, so `handshake` comes back empty: the
        # numeric test below is what skips it. Its first field is the private
        # key, never the interface name, so matching on $WG_IFACE would not.
        [[ "$handshake" =~ ^[0-9]+$ ]] || continue
        [[ "$tx" =~ ^[0-9]+$ ]] || continue
        prev=${WG_PREV_TX[$peer]:-$tx}
        if [ "$tx" -gt "$prev" ]; then
            WG_EMITTED[$peer]=1
            [ -n "${WG_SUSPECT_SINCE[$peer]:-}" ] || WG_SUSPECT_SINCE[$peer]=$now
        fi
        WG_PREV_TX[$peer]=$tx
        [ "${WG_EMITTED[$peer]:-0}" -eq 1 ] || continue
        emitters=$((emitters + 1))
        if [ "$handshake" -gt 0 ]; then age=$((now - handshake)); else age=$((now - ${WG_SUSPECT_SINCE[$peer]:-$now})); fi
        suspect_since=${WG_SUSPECT_SINCE[$peer]:-$now}
        if [ "$age" -le 130 ]; then
            state=healthy
        elif [ "$age" -ge 180 ] && [ $((now - suspect_since)) -ge 25 ]; then
            state=down
        else
            state=suspect
        fi
        if [ "${WG_STATE[$peer]:-}" != "$state" ]; then
            WG_STATE[$peer]=$state
        fi
        case "$state" in healthy) healthy=$((healthy+1));; suspect) suspect=$((suspect+1));; down) down=$((down+1));; esac
    done <<< "$dump"
    [ "$emitters" -gt 0 ] || return
    if [ "$down" -eq "$emitters" ]; then
        set_global_health down ALL_EMITTING_PEERS_DOWN
    elif [ "$suspect" -gt 0 ] || [ "$down" -gt 0 ]; then
        set_global_health suspect PEER_HANDSHAKE_STALE
    else
        set_global_health healthy
    fi
}

handle_command() {
    local command=$1 attempt failure_detail
    case "$command" in
        ACTIVE) ACTIVE=1 ;;
        # WG_PREV_TX goes with the rest: a counter captured before the pause
        # would make the first poll after ACTIVE see a jump that happened while
        # nothing was being posted, and start the liveness clock on it.
        IDLE) ACTIVE=0; WG_PREV_TX=(); WG_EMITTED=(); WG_SUSPECT_SINCE=(); WG_STATE=() ;;
        STOP|stop) return 1 ;;
        RESTART\ attempt_id=*)
            attempt=${command#RESTART attempt_id=}
            if ! [[ "$attempt" =~ ^[0-9]+$ ]]; then
                v2_emit "RESTART_FAILED attempt_id=0 failure=configuration detail=$(percent_encode "invalid attempt id")"
                return 0
            fi
            cleanup_resources
            if ! start_tunnel "$attempt"; then
                failure_detail=${LAST_ERROR:-restart failed}
                # A failed reconstruction can already have created an
                # interface, route or child process. Remove that partial
                # attempt before entering the manager's backoff window.
                cleanup_resources
                v2_emit "RESTART_FAILED attempt_id=$attempt failure=tunnel_lost detail=$(percent_encode "$failure_detail")"
            fi
            ;;
    esac
    return 0
}

# Timeout keeps stdin integrated into supervision. rc=1 means actual EOF;
# timeout return codes are greater than 128 and merely trigger a health poll.
while true; do
    command=""
    if IFS= read -r -t 1 command; then
        handle_command "$command" || break
    else
        rc=$?
        [ "$rc" -ne 1 ] || break
    fi
    if [ "$ACTION" = openvpn ]; then poll_openvpn; else poll_wireguard; fi
done

exit 0
