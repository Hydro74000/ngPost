# ngPost depends on qtkeychain-qt6 (libqt6keychain). Building with Qt 5
# silently produces a broken binary because libQt6Core is pulled in via the
# keychain dependency but never appears in the link line, so ld --as-needed
# rejects it. Fail loudly here instead of waiting for the linker to crash.
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
TARGET = ngPost
TEMPLATE = app
CONFIG += c++17
CONFIG -= app_bundle

DEFINES += __USE_CONNECTION_TIMEOUT__
DEFINES += __COMPUTE_IMMEDIATE_SPEED__

DEFINES += __USE_TMP_RAM__

DEFINES += __RELEASE_ARTICLES_WHEN_CON_FAILS__


# macro for debuging posting on multiple provides (no need anymore)
#DEFINES -= __DISP_ARTICLE_SERVER__


TRANSLATIONS = lang/ngPost_en.ts lang/ngPost_fr.ts lang/ngPost_es.ts lang/ngPost_de.ts\
               lang/ngPost_nl.ts lang/ngPost_pt.ts lang/ngPost_zh.ts

win32: {
    # ws2_32 is needed by the Windows VPN bind helper.
    LIBS += -luser32 -ladvapi32 -lws2_32
    RC_ICONS = ngPost.ico
    RC_FILE = resources/version.rc
    # Include console only if not using HMI (GUI)
    CONFIG += console
}

macx: {
    ICON = ngPost.icns
    CONFIG += app_bundle

    # Ensure par2 and parpar exist externally before adding them
    system(test -f /usr/local/bin/par2) {
        ExtraFiles.files = /usr/local/bin/par2 /usr/local/bin/parpar $$PWD/LICENSE
        ExtraFiles.path = Contents/MacOS
        QMAKE_BUNDLE_DATA += ExtraFiles
    }
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

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        ArticleBuilder.cpp \
        FileUploader.cpp \
        FoldersMonitorForNewFiles.cpp \
        NgPost.cpp \
        NntpCheckCon.cpp \
        NntpConnection.cpp \
        NzbCheck.cpp \
        Poster.cpp \
        PostingJob.cpp \
        main.cpp \
        nntp/Nntp.cpp \
        nntp/NntpArticle.cpp \
        nntp/NntpFile.cpp \
        utils/CmdOrGuiApp.cpp \
        utils/PathHelper.cpp \
        utils/UpdateChecker.cpp \
        utils/Yenc.cpp \
        vpn/VpnDnsResolver.cpp \
        vpn/OpenVpnBackend.cpp \
        vpn/VpnManager.cpp \
        vpn/VpnProfile.cpp \
        vpn/VpnSocketBinder.cpp \
        vpn/WindowsBindHelper.cpp \
        vpn/WireGuardBackend.cpp


# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# Runtime VPN resources: helper + install/uninstall scripts + polkit rule
# template. Installed under /var/lib/ngpost/ which is writable on traditional
# AND atomic distros (Bazzite/Silverblue/Kinoite/SteamOS, where /usr is RO).
# Files are root-owned 755 by the install command — this is what makes the
# strict-path polkit rule safe.
unix:!android:!macx {
    vpn_runtime.files = $$PWD/vpn/scripts/ngpost-vpn-helper.sh \
                        $$PWD/vpn/scripts/ngpost-vpn-install.sh \
                        $$PWD/vpn/scripts/ngpost-vpn-uninstall.sh \
                        $$PWD/vpn/polkit/49-ngpost-vpn.rules.in
    vpn_runtime.path  = /var/lib/ngpost
    INSTALLS += vpn_runtime
}

HEADERS += \
    ArticleBuilder.h \
    FileUploader.h \
    FoldersMonitorForNewFiles.h \
    NgPost.h \
    NntpCheckCon.h \
    NntpConnection.h \
    NzbCheck.h \
    Poster.h \
    PostingJob.h \
    nntp/Nntp.h \
    nntp/NntpArticle.h \
    nntp/NntpFile.h \
    nntp/NntpServerParams.h \
    utils/CmdOrGuiApp.h \
    utils/Macros.h \
    utils/PathHelper.h \
    utils/PureStaticClass.h \
    utils/UpdateChecker.h \
    utils/Yenc.h \
    vpn/VpnDnsResolver.h \
    vpn/OpenVpnBackend.h \
    vpn/VpnBackend.h \
    vpn/VpnManager.h \
    vpn/VpnProfile.h \
    vpn/VpnSocketBinder.h \
    vpn/WindowsBindHelper.h \
    vpn/WireGuardBackend.h



RESOURCES += \
    resources/resources.qrc \
    resources/version.rc
