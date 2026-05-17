# tests/vpn/vpn.pro — SUBDIRS for VPN end-to-end tests.
#
# These bring up real WireGuard / OpenVPN tunnels via shell helpers,
# then run ngPost through them and assert on the peer IP recorded by the
# mock NNTP server. They require:
#   * Linux with the wireguard kernel module
#   * wireguard-tools, iproute2 installed
#   * passwordless sudo (or set NGPOST_VPN_E2E=skip to opt out)
#
# Skipped automatically when those prerequisites are missing.

TEMPLATE = subdirs

SUBDIRS = \
    tst_VpnE2E_WireGuard
