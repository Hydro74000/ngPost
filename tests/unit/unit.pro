# tests/unit/unit.pro — SUBDIRS of QTest binaries (one tst_* per subdirectory).

TEMPLATE = subdirs

SUBDIRS = \
    tst_Yenc \
    tst_PathHelper \
    tst_PostHistory \
    tst_PostInfoTemplate \
    tst_VpnProfile \
    tst_CliParser \
    tst_UpdateChecker \
    tst_WireGuardBackend \
    tst_WindowsBindHelper
