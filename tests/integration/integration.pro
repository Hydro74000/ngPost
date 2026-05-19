# tests/integration/integration.pro — SUBDIRS for tests that depend on the
# mock NNTP server (Python child process) or other external fixtures.
#
# Kept separate from tests/unit/ because unit tests must pass without
# python3 on PATH, while these are skipped in that scenario.

TEMPLATE = subdirs

SUBDIRS = \
    tst_MockNntp \
    tst_PostFlow \
    tst_VpnDnsResolver \
    tst_GoldenNzb
