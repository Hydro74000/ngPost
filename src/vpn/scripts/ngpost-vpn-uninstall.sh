#!/bin/bash
# ngpost-vpn-uninstall — removes the privileged VPN helper and its polkit rule.
# Symmetric to ngpost-vpn-install.sh.

set -u

rm -f /var/lib/ngpost/ngpost-vpn-helper.sh
rm -f /var/lib/ngpost/ngpost-vpn-uninstall.sh
rm -f /var/lib/ngpost/bin/openvpn /var/lib/ngpost/bin/wireguard-go /var/lib/ngpost/bin/wg
rmdir --ignore-fail-on-non-empty /var/lib/ngpost/bin 2>/dev/null || true
rmdir --ignore-fail-on-non-empty /var/lib/ngpost 2>/dev/null || true

rm -f /etc/polkit-1/rules.d/49-ngpost-vpn.rules

echo "UNINSTALLED"
