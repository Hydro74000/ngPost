# tests/unit/unit.pro — SUBDIRS of QTest binaries (one tst_* per subdirectory).

TEMPLATE = subdirs

SUBDIRS = \
    tst_Par2Settings \
    tst_WindowsServiceControl \
    tst_WindowsCommandLine \
    tst_RandomToken \
    tst_Yenc \
    tst_NntpFile \
    tst_VpnSocketBinder \
    tst_PathHelper \
    tst_PostHistory \
    tst_PostInfoTemplate \
    tst_VpnProfile \
    tst_OpenVpnConfigPolicy \
    tst_WireGuardConfigPolicy \
    tst_CliParser \
    tst_FoldersMonitor \
    tst_UpdateChecker \
    tst_WireGuardBackend \
    tst_WindowsBindHelper \
    tst_WindowsSecurity
