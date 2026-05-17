# GUI tests need use_hmi → the hmi/ sources from ngPost_core.pri are
# pulled into the build, along with QtWidgets / QtGui.
CONFIG += use_hmi
include(../../common/common.pri)

# Qt resources for icons referenced by the .ui files. Without these the
# tests still run (icons are silently empty) but linking against the qrc
# keeps the binary self-contained.
RESOURCES += $$PWD/../../../src/resources/resources.qrc

TARGET   = tst_MainWindow
SOURCES += tst_MainWindow.cpp
