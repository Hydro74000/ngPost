#!/bin/bash
# ngpost-vpn-install — installs the privileged VPN helper for a single user.
# Invoked by ngPost as: pkexec ngpost-vpn-install.sh <source-dir>
# <source-dir> must contain: ngpost-vpn-helper.sh, ngpost-vpn-uninstall.sh,
# and 49-ngpost-vpn.rules.in (the polkit rule template).
#
# Result: the helper lands in /var/lib/ngpost/ (root-owned, root-only-writable
# so the polkit "no prompt" rule can safely whitelist its absolute path), and
# a polkit rule in /etc/polkit-1/rules.d/ grants `polkit.Result.YES` to that
# exact path for the invoking user only.
#
# Path choice: /var/lib is writable on every distro including atomic ones
# (Bazzite, Silverblue, Kinoite, SteamOS) where /usr/ is read-only ostree.

set -euo pipefail

# Authentication must precede revocation. Once elevated, fail closed even if
# the supplied resources are missing: never leave the old passwordless helper
# reachable after an unsuccessful upgrade. Atomic replacement also protects
# against Polkit retaining its old rule briefly while reloading.
[ "$(id -u)" = 0 ] && [ -n "${PKEXEC_UID:-}" ] \
    || { echo "ERROR installer requires administrator authentication via pkexec"; exit 1; }
install -d -m 755 -o root -g root /var/lib/ngpost
disabled=$(mktemp /var/lib/ngpost/.helper-disabled.XXXXXX)
chmod 0600 "$disabled"
mv -fT -- "$disabled" /var/lib/ngpost/ngpost-vpn-helper.sh
rm -f -- /etc/polkit-1/rules.d/49-ngpost-vpn.rules

SRC="${1:-}"
[ -n "$SRC" ] || { echo "ERROR usage: $0 <source-dir>"; exit 1; }
[ -d "$SRC" ] || { echo "ERROR source dir not found: $SRC"; exit 1; }
[ -r "$SRC/ngpost-vpn-helper.sh"      ] || { echo "ERROR missing helper.sh in $SRC";    exit 1; }
[ -r "$SRC/ngpost-vpn-uninstall.sh"   ] || { echo "ERROR missing uninstall.sh in $SRC"; exit 1; }
[ -r "$SRC/49-ngpost-vpn.rules.in"    ] || { echo "ERROR missing polkit rule template"; exit 1; }
grep -qx 'readonly NGPOST_VPN_HELPER_SECURITY_REVISION=3' "$SRC/ngpost-vpn-helper.sh" \
    || { echo "ERROR helper security revision 3 required"; exit 1; }
bash -n "$SRC/ngpost-vpn-helper.sh"

# Must run under pkexec — that's how we discover who is asking.
[ -n "${PKEXEC_UID:-}" ] || { echo "ERROR no PKEXEC_UID (must run via pkexec)"; exit 1; }
USER_NAME=$(getent passwd "$PKEXEC_UID" | cut -d: -f1)
[ -n "$USER_NAME" ] || { echo "ERROR cannot resolve PKEXEC_UID=$PKEXEC_UID"; exit 1; }

# Install runtime scripts. /var/lib stays writable on atomic distros.
install -d -m 755 /var/lib/ngpost
staged_helper=$(mktemp /var/lib/ngpost/.helper-new.XXXXXX)
trap 'rm -f -- "$staged_helper"' EXIT
install -m 755 -o root -g root "$SRC/ngpost-vpn-helper.sh" "$staged_helper"
mv -fT -- "$staged_helper" /var/lib/ngpost/ngpost-vpn-helper.sh
install -m 755 -o root -g root "$SRC/ngpost-vpn-uninstall.sh" /var/lib/ngpost/
if [ -d "$SRC/bin" ]; then
    install -d -m 755 -o root -g root /var/lib/ngpost/bin
    for tool in openvpn wireguard-go wg; do
        [ ! -f "$SRC/bin/$tool" ] \
            || install -m 755 -o root -g root "$SRC/bin/$tool" "/var/lib/ngpost/bin/$tool"
    done
fi

# SELinux: relabel as executable so pkexec accepts to run them on Fedora atomic
# and similar enforcing systems. Quietly skipped where SELinux is absent.
chcon -t bin_t /var/lib/ngpost/ngpost-vpn-helper.sh    2>/dev/null || true
chcon -t bin_t /var/lib/ngpost/ngpost-vpn-uninstall.sh 2>/dev/null || true
restorecon -F /var/lib/ngpost/*.sh                      2>/dev/null || true
restorecon -RF /var/lib/ngpost/bin                      2>/dev/null || true

# Install the per-user polkit rule. /etc is writable on atomic too.
install -d -m 755 /etc/polkit-1/rules.d
sed "s|@@USER@@|$USER_NAME|g" "$SRC/49-ngpost-vpn.rules.in" \
    > /etc/polkit-1/rules.d/49-ngpost-vpn.rules
chmod 644 /etc/polkit-1/rules.d/49-ngpost-vpn.rules
chown root:root /etc/polkit-1/rules.d/49-ngpost-vpn.rules

echo "INSTALLED user=$USER_NAME helper=/var/lib/ngpost/ngpost-vpn-helper.sh"
