# tests/tests.pro — top-level SUBDIRS for all test categories.
#
# Build with:  qmake tests.pro && make -j
# Run via:     find . -type f -executable -name 'tst_*' -exec {} \;
#
# Categories are added one phase at a time:
#   Phase 2 → unit/
#   Phase 3 → cli/ (CLI integration with mock NNTP)
#   Phase 4 → gui/
#   Phase 5 → vpn/ (WireGuard / OpenVPN E2E)

TEMPLATE = subdirs

SUBDIRS = unit integration gui vpn
