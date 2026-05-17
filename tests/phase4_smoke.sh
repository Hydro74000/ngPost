#!/bin/bash
# Phase 4 automated smoke tests for the VPN profile machinery.
#
# Exercises ngPost's startup-time behavior (parse + migration + save) without
# touching the GUI. Each test runs ngPost briefly with HOME/XDG redirected to
# a sandbox, then inspects the resulting filesystem state.
#
# Run from the repo root:  bash tests/phase4_smoke.sh

set -u

BIN=${NGPOST_BIN:-$(dirname "$0")/../src/ngPost}
if [ ! -x "$BIN" ]; then
    echo "FAIL: ngPost binary not found at $BIN"
    exit 1
fi

pass=0
fail=0
ok()   { printf "  \033[32mPASS\033[0m %s\n" "$*"; pass=$((pass+1)); }
ko()   { printf "  \033[31mFAIL\033[0m %s\n" "$*"; fail=$((fail+1)); }

run_ngpost() {
    # Run ngPost briefly inside a sandboxed HOME. We use --version because it
    # triggers the constructor (which runs PathHelper::migrateLegacyConfigIfNeeded
    # and parseDefaultConfig) but exits immediately afterwards. Some output is
    # discarded; we care about the filesystem side effects.
    local h="$1"; shift
    HOME="$h" XDG_CONFIG_HOME="$h/.config" \
        "$BIN" --version "$@" > "$h/ngpost.out" 2>&1 || true
}

# -------------------------------------------------------------------------
# Test 1: migration of legacy ~/.ngPost into XDG location.
# -------------------------------------------------------------------------
echo "Test 1: migration ~/.ngPost -> ~/.config/ngPost/ngPost.conf"
T1=$(mktemp -d)
cat > "$T1/.ngPost" <<'EOF'
lang = EN
[server]
host = test1.example.com
port = 119
ssl  = false
user = u1
pass = p1
connection = 10
enabled = true
nzbCheck = false
EOF

run_ngpost "$T1"

newConf="$T1/.config/ngPost/ngPost.conf"
[ -f "$newConf" ] || ko "new conf not present at $newConf"
[ -f "$newConf" ] && ok "new conf created"
[ ! -f "$T1/.ngPost" ] && ok "legacy ~/.ngPost removed" \
    || ko "legacy ~/.ngPost still present (should be moved)"
grep -q "host = test1.example.com" "$newConf" && ok "server preserved" \
    || ko "server entry missing from migrated conf"

rm -rf "$T1"

# -------------------------------------------------------------------------
# Test 2: round-trip — [vpn_profile] block parsed and re-emitted.
# -------------------------------------------------------------------------
echo "Test 2: vpn_profile roundtrip"
T2=$(mktemp -d)
mkdir -p "$T2/.config/ngPost/vpn"
echo "# fake openvpn config" > "$T2/.config/ngPost/vpn/myprof.ovpn"
cat > "$T2/.config/ngPost/ngPost.conf" <<'EOF'
lang = EN
VPN_AUTO_CONNECT = true
VPN_ACTIVE_PROFILE = MyProf

[vpn_profile]
name        = MyProf
backend     = openvpn
config_file = myprof.ovpn
has_auth    = true

[server]
host = test2.example.com
port = 563
ssl  = true
user = u2
pass = p2
connection = 5
enabled = true
nzbCheck = false
useVpn = true
EOF

# Trigger a re-save by forcing a configChanged: ngPost will re-emit the conf
# if the auto-save is triggered. With --version it isn't; the file should
# remain UNCHANGED (no wipe). Verify nothing was lost.
cp "$T2/.config/ngPost/ngPost.conf" "$T2/conf.before"
run_ngpost "$T2"
diff -q "$T2/conf.before" "$T2/.config/ngPost/ngPost.conf" >/dev/null \
    && ok "conf untouched on read-only run" \
    || ko "conf was rewritten on --version run (signals should be blocked)"
grep -q "^\[vpn_profile\]" "$T2/.config/ngPost/ngPost.conf" \
    && ok "[vpn_profile] block preserved" \
    || ko "[vpn_profile] block was stripped"
grep -q "useVpn = true" "$T2/.config/ngPost/ngPost.conf" \
    && ok "per-server useVpn preserved" \
    || ko "per-server useVpn was stripped"

rm -rf "$T2"

# -------------------------------------------------------------------------
# Test 3: legacy single-profile (VPN_CONFIG_PATH) is migrated to a [vpn_profile].
# -------------------------------------------------------------------------
echo "Test 3: legacy VPN_CONFIG_PATH -> Default profile"
T3=$(mktemp -d)
echo "# legacy ovpn" > "$T3/old.ovpn"
cat > "$T3/.ngPost" <<EOF
lang = EN
VPN_BACKEND = openvpn
VPN_CONFIG_PATH = $T3/old.ovpn

[server]
host = legacy.example.com
port = 119
ssl  = false
EOF

run_ngpost "$T3"

# The migrate-then-parse path copies the legacy ovpn into vpn/ AND creates
# a Default profile in memory. The profile is only persisted to disk if a
# saveConfig() fires — which doesn't happen on a --version-only run. So we
# only check the file copy here, plus the legacy file presence/absence.
[ -f "$T3/.config/ngPost/vpn/old.ovpn" ] \
    && ok "legacy .ovpn copied into <configDir>/vpn/" \
    || ko "legacy .ovpn NOT copied (expected $T3/.config/ngPost/vpn/old.ovpn)"
[ ! -f "$T3/.ngPost" ] \
    && ok "legacy ~/.ngPost removed" \
    || ko "legacy ~/.ngPost still present"

rm -rf "$T3"

# -------------------------------------------------------------------------
# Test 4: helper script cleanup mode is idempotent (no-op when nothing stale)
# -------------------------------------------------------------------------
echo "Test 4: helper script cleanup mode on clean state"
HELPER=/var/lib/ngpost/ngpost-vpn-helper.sh
if [ -x "$HELPER" ]; then
    out=$(pkexec --disable-internal-agent "$HELPER" cleanup 2>&1 < /dev/null || true)
    if echo "$out" | grep -q "NOTHING_TO_CLEAN"; then
        ok "cleanup mode reports NOTHING_TO_CLEAN on clean system"
    elif echo "$out" | grep -q "CLEANED"; then
        ok "cleanup mode succeeded (something was found to clean)"
    else
        ko "cleanup mode unexpected output: $out"
    fi
else
    echo "  SKIP installed helper not present (run install first)"
fi

# -------------------------------------------------------------------------
echo
echo "Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
