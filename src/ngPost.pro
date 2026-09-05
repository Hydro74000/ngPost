# ngPost (GUI build) entry point.
#
# Toggle `use_hmi` off for a headless CLI build. Note that the released
# binary is this one: it serves both the GUI and the command line.
# All the heavy lifting — sources, defines, Qt modules, install rules —
# lives in ngPost.pri / ngPost_core.pri.

CONFIG  += use_hmi

use_hmi {
    QT += gui
    greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

    DEFINES += __USE_HMI__
}
else {
    QT -= gui
    CONFIG += console
}

include(ngPost.pri)
