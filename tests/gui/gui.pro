# tests/gui/gui.pro — SUBDIRS for offscreen GUI tests.
#
# These tests need QtWidgets and the hmi/ sources (set CONFIG+=use_hmi
# before include(common.pri) in each per-test .pro). They run under
# QT_QPA_PLATFORM=offscreen so no display server is required.

TEMPLATE = subdirs

SUBDIRS = \
    tst_MainWindow

# Phase 4.x — add when the VpnManager/Keychain test seam is in place:
#   tst_VpnProfileEditDialog
#   tst_VpnSettingsDialog
#   tst_PostingWidget
#   tst_AutoPostWidget
