# ngPost.pri — application composition.
#
# Builds the ngPost executable on top of the shared core sources defined in
# ngPost_core.pri. Anything that is intrinsically part of the binary (main.cpp,
# RESOURCES, install rules, Windows icon/manifest, macOS app bundle, polkit
# helper install) lives here, not in ngPost_core.pri, so that test targets
# pulling in ngPost_core.pri don't drag in app-only artefacts.

include($$PWD/ngPost_core.pri)

TARGET = ngPost
TEMPLATE = app
CONFIG -= app_bundle

# main.cpp is the real entry point; the rest of the source list comes from
# ngPost_core.pri above.
SOURCES += $$PWD/main.cpp

win32: {
    RC_ICONS = ngPost.ico
    RC_FILE = $$PWD/resources/version.rc
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

# Qt resources (icons, translation .qm files). Kept on the app side rather
# than in ngPost_core.pri so test binaries that link the core sources don't
# need to satisfy the resource init at startup.
RESOURCES += $$PWD/resources/resources.qrc
win32: RESOURCES += $$PWD/resources/version.rc
