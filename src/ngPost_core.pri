# ngPost_core.pri — shared source + settings module.
#
# Lists the C++ sources, Qt modules, defines, and platform-specific link
# requirements that make up ngPost minus its `main.cpp`. Both the app build
# (via src/ngPost.pri) and the tests under tests/ include this file so the
# same compilation flags and source set are used everywhere.
#
# Paths use $$PWD so the file can be safely included from any subdirectory
# (e.g. tests/unit/tst_VpnProfile/).

lessThan(QT_MAJOR_VERSION, 6): error("ngPost requires Qt 6 (qtkeychain-qt6). Use qmake6, not qmake/qmake-qt5.")

QT += core network

# Cross-platform credential store (QtKeychain) for OpenVPN auth.
# Packages: Fedora qtkeychain-qt6, Ubuntu libqt6keychain1-dev, brew qtkeychain.
# Header is <qt6keychain/keychain.h>; needs DBus on Linux.
#
# In CI we build QtKeychain from source and install it into $QT_ROOT_DIR.
# Add that to the search paths so the include / link resolves on macOS
# (qmake doesn't auto-add $QT_ROOT_DIR/include to the include path the way
# Linux distros place QtKeychain in /usr/include).
QT_KEYCHAIN_PREFIX = $$(QT_ROOT_DIR)
!isEmpty(QT_KEYCHAIN_PREFIX) {
    INCLUDEPATH += $$QT_KEYCHAIN_PREFIX/include
    LIBS        += -L$$QT_KEYCHAIN_PREFIX/lib
}
LIBS    += -lqt6keychain
QT      += dbus

VERSION = 5.2.2
DEFINES += APP_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD
CONFIG += c++17

DEFINES += __USE_CONNECTION_TIMEOUT__
DEFINES += __COMPUTE_IMMEDIATE_SPEED__

DEFINES += __USE_TMP_RAM__

DEFINES += __RELEASE_ARTICLES_WHEN_CON_FAILS__

# macro for debuging posting on multiple provides (no need anymore)
#DEFINES -= __DISP_ARTICLE_SERVER__

TRANSLATIONS = $$PWD/lang/ngPost_en.ts $$PWD/lang/ngPost_fr.ts $$PWD/lang/ngPost_es.ts $$PWD/lang/ngPost_de.ts\
               $$PWD/lang/ngPost_nl.ts $$PWD/lang/ngPost_pt.ts $$PWD/lang/ngPost_zh.ts

win32: {
    # ws2_32 is needed by the Windows VPN bind helper.
    LIBS += -luser32 -ladvapi32 -lws2_32
}

CONFIG(debug, debug|release) :{
    DEFINES += __DEBUG__

    DEFINES += LOG_CONNECTION_STEPS
    DEFINES -= LOG_CONNECTION_ERRORS_BEFORE_EMIT_SIGNALS
    DEFINES += LOG_NEWS_AUTH
    DEFINES -= LOG_NEWS_DATA
    DEFINES += LOG_CONSTRUCTORS

    DEFINES -= __SAVE_ARTICLES__
}
else {
    # In release mode, remove all qDebugs !
    DEFINES += QT_NO_DEBUG_OUTPUT
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# Core sources (no main.cpp — that lives in the app .pro).
SOURCES += \
        $$PWD/ArticleBuilder.cpp \
        $$PWD/FileUploader.cpp \
        $$PWD/FoldersMonitorForNewFiles.cpp \
        $$PWD/NgPost.cpp \
        $$PWD/NntpCheckCon.cpp \
        $$PWD/NntpConnection.cpp \
        $$PWD/NzbCheck.cpp \
        $$PWD/Poster.cpp \
        $$PWD/PostingJob.cpp \
        $$PWD/nntp/Nntp.cpp \
        $$PWD/nntp/NntpArticle.cpp \
        $$PWD/nntp/NntpFile.cpp \
        $$PWD/utils/CmdOrGuiApp.cpp \
        $$PWD/utils/PathHelper.cpp \
        $$PWD/utils/UpdateChecker.cpp \
        $$PWD/utils/Yenc.cpp \
        $$PWD/vpn/VpnDnsResolver.cpp \
        $$PWD/vpn/OpenVpnBackend.cpp \
        $$PWD/vpn/VpnManager.cpp \
        $$PWD/vpn/VpnProfile.cpp \
        $$PWD/vpn/VpnSocketBinder.cpp \
        $$PWD/vpn/WindowsBindHelper.cpp \
        $$PWD/vpn/WireGuardBackend.cpp

HEADERS += \
    $$PWD/ArticleBuilder.h \
    $$PWD/FileUploader.h \
    $$PWD/FoldersMonitorForNewFiles.h \
    $$PWD/NgPost.h \
    $$PWD/NntpCheckCon.h \
    $$PWD/NntpConnection.h \
    $$PWD/NzbCheck.h \
    $$PWD/Poster.h \
    $$PWD/PostingJob.h \
    $$PWD/nntp/Nntp.h \
    $$PWD/nntp/NntpArticle.h \
    $$PWD/nntp/NntpFile.h \
    $$PWD/nntp/NntpServerParams.h \
    $$PWD/utils/CmdOrGuiApp.h \
    $$PWD/utils/Macros.h \
    $$PWD/utils/PathHelper.h \
    $$PWD/utils/PureStaticClass.h \
    $$PWD/utils/UpdateChecker.h \
    $$PWD/utils/Yenc.h \
    $$PWD/vpn/VpnDnsResolver.h \
    $$PWD/vpn/OpenVpnBackend.h \
    $$PWD/vpn/VpnBackend.h \
    $$PWD/vpn/VpnManager.h \
    $$PWD/vpn/VpnProfile.h \
    $$PWD/vpn/VpnSocketBinder.h \
    $$PWD/vpn/WindowsBindHelper.h \
    $$PWD/vpn/WireGuardBackend.h

# HMI sources are pulled in only when the consumer set `CONFIG += use_hmi`
# before including this file (see src/ngPost.pri). The CLI-only build and
# headless test binaries leave use_hmi off and skip the QtWidgets dep.
#
# The Qt module list + __USE_HMI__ flag live here too so that GUI test
# .pro files only have to do `CONFIG += use_hmi ; include(common.pri)` and
# everything resolves consistently.
use_hmi {
QT += gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets
DEFINES += __USE_HMI__

SOURCES += \
    $$PWD/hmi/AutoPostWidget.cpp \
    $$PWD/hmi/CheckBoxCenterWidget.cpp \
    $$PWD/hmi/PostingWidget.cpp \
    $$PWD/hmi/SignedListWidget.cpp \
    $$PWD/hmi/MainWindow.cpp \
    $$PWD/hmi/VpnProfileEditDialog.cpp \
    $$PWD/hmi/VpnSettingsDialog.cpp

HEADERS += \
    $$PWD/hmi/AutoPostWidget.h \
    $$PWD/hmi/CheckBoxCenterWidget.h \
    $$PWD/hmi/PostingWidget.h \
    $$PWD/hmi/SignedListWidget.h \
    $$PWD/hmi/MainWindow.h \
    $$PWD/hmi/VpnProfileEditDialog.h \
    $$PWD/hmi/VpnSettingsDialog.h

FORMS += \
    $$PWD/hmi/AutoPostWidget.ui \
    $$PWD/hmi/MainWindow.ui \
    $$PWD/hmi/PostingWidget.ui \
    $$PWD/hmi/VpnProfileEditDialog.ui \
    $$PWD/hmi/VpnSettingsDialog.ui
}
