#!/bin/bash
# Contract tests for the privileged helper's lease protocol. Run as root in
# the isolated VPN E2E job; no tunnel is created by these checks.

set -euo pipefail

HELPER=${HELPER:-src/vpn/scripts/ngpost-vpn-helper.sh}
LOCK_FILE=/run/lock/ngpost-vpn.lock
OWNER_FILE=/run/ngpost-vpn/owner-v2
TEST_TMP=$(mktemp -d /tmp/ngpost-helper-lease.XXXXXX)

cleanup() {
    rm -f "$OWNER_FILE"
    rm -f "$LOCK_FILE"
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT

proc_start_time() {
    local stat rest
    local -a fields
    stat=$(cat "/proc/$1/stat")
    rest=${stat##*) }
    read -r -a fields <<< "$rest"
    printf '%s' "${fields[19]}"
}

owner_pid=$$
owner_start=$(proc_start_time "$$")
mkdir -p /run/lock /run/ngpost-vpn
rm -f "$OWNER_FILE" "$LOCK_FILE"

# Reject malformed numeric input before lease arithmetic or any network
# command. The v2 protocol header must remain the first record.
set +e
"$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
    --owner-start "$owner_start" --wait-minutes '1+bad' >"$TEST_TMP/invalid-number.out"
invalid_number_code=$?
"$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
    --owner-start "$owner_start" --wait-minutes 1441 >"$TEST_TMP/out-of-range.out"
out_of_range_code=$?
set -e
[ "$invalid_number_code" -eq 1 ]
[ "$out_of_range_code" -eq 1 ]
[ "$(head -n 1 "$TEST_TMP/invalid-number.out")" = 'PROTOCOL 2' ]
grep -Eq '^ERROR failure=configuration detail=' "$TEST_TMP/invalid-number.out"
grep -Eq '^ERROR failure=configuration detail=' "$TEST_TMP/out-of-range.out"

# An explicit executable directory crosses the privilege boundary.  Reject it
# before touching the lease when either the directory or one of its tools can
# be modified by an unprivileged user.
unsafe_bin="$TEST_TMP/unsafe-bin"
mkdir "$unsafe_bin"
chmod 0777 "$unsafe_bin"
set +e
"$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
    --owner-start "$owner_start" --bin-dir "$unsafe_bin" \
    --nonblocking >"$TEST_TMP/unsafe-bin.out"
unsafe_bin_code=$?
set -e
[ "$unsafe_bin_code" -eq 1 ]
[ "$(head -n 1 "$TEST_TMP/unsafe-bin.out")" = 'PROTOCOL 2' ]
grep -Eq '^ERROR failure=configuration detail=' "$TEST_TMP/unsafe-bin.out"
[ ! -e "$LOCK_FILE" ]

# Hold a valid deliberately-long owner record. A second helper must never
# mutate state and must expose all diagnostic owner fields through BUSY.
exec {HOLD_FD}<>"$LOCK_FILE"
flock -n "$HOLD_FD"
long_backend=$(printf 'a%.0s' {1..300})
long_record="v=1 owner_pid=123 owner_start=456 owner_uid=789 helper_pid=321 backend=$long_backend since=111 end=1"
truncate -s 0 "/proc/self/fd/$HOLD_FD"
printf '%s' "$long_record" >&"$HOLD_FD"

set +e
"$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
    --owner-start "$owner_start" --nonblocking >"$TEST_TMP/busy.out"
busy_code=$?
set -e
[ "$busy_code" -eq 2 ]
grep -Fxq 'PROTOCOL 2' "$TEST_TMP/busy.out"
grep -Eq '^BUSY owner_pid=123 helper_pid=321 owner_uid=789 backend=' "$TEST_TMP/busy.out"
[ "$(grep -c '^BUSY ' "$TEST_TMP/busy.out")" -eq 1 ]

# A waiting CLI invocation reports WAITING immediately. Once the owner drops
# the flock, the same elevated process acquires it and completes normally.
(
    # The waiter must not inherit the fixture descriptor which deliberately
    # owns the lease. Real competing ngPost processes never share that open
    # file description; keeping it here would make this test lock itself.
    exec {HOLD_FD}>&-
    exec "$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
        --owner-start "$owner_start" --wait-minutes 1
) >"$TEST_TMP/waiting.out" &
waiter=$!
for _ in {1..50}; do
    grep -q '^WAITING owner_pid=123 helper_pid=321 owner_uid=789 ' "$TEST_TMP/waiting.out" \
        && break
    sleep 0.1
done
grep -q '^WAITING owner_pid=123 helper_pid=321 owner_uid=789 ' "$TEST_TMP/waiting.out"
exec {HOLD_FD}>&-
wait "$waiter"
grep -Fxq 'NOTHING_TO_CLEAN' "$TEST_TMP/waiting.out"

# The new, shorter record must have replaced the long record completely.
# This catches missing ftruncate() and partial metadata publication.
record=$(cat "$LOCK_FILE")
[[ "$record" =~ ^v=1\ owner_pid=[0-9]+\ owner_start=[0-9]+\ owner_uid=[0-9]+\ helper_pid=[0-9]+\ backend=cleanup-stale\ since=[0-9]+\ end=1$ ]]
[ "$(grep -o 'end=1' <<< "$record" | wc -l)" -eq 1 ]
[ "${#record}" -lt "${#long_record}" ]

# A malformed/partial manifest never authorises cleanup. It produces the
# named protocol result and is left for explicit human diagnosis.
printf '%s' 'v=2 owner_pid=1 helper_pid=2 backend=wireguard end=1 trailing' >"$OWNER_FILE"
set +e
"$HELPER" cleanup-stale --protocol 2 --owner-pid "$owner_pid" \
    --owner-start "$owner_start" --nonblocking >"$TEST_TMP/malformed.out"
malformed_code=$?
set -e
[ "$malformed_code" -eq 3 ]
grep -q '^UNATTRIBUTED_VPN_STATE resources=malformed_manifest$' "$TEST_TMP/malformed.out"
grep -Fq 'trailing' "$OWNER_FILE"

echo 'helper lease contract: PASS'
