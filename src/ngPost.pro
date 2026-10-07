# ngPost entry point, graphical by default.
#
#   qmake6                     -> the released binary: GUI and command line
#   qmake6 CONFIG+=no_hmi      -> headless build, for a server or a container
#
# The headless one links Qt Core, Network, Sql, DBus and qtkeychain only -- no
# Gui, no Widgets, no OpenGL, no X11 -- and runs with no DISPLAY. The switch is
# read before `CONFIG += use_hmi` below on purpose: qmake applies command-line
# assignments before the project file, so a plain `CONFIG-=use_hmi` would be
# undone by the line that follows, and `-after` comes too late to matter -- the
# use_hmi block has already expanded.
#
# All the heavy lifting — sources, defines, Qt modules, install rules —
# lives in ngPost.pri / ngPost_core.pri.

!no_hmi: CONFIG += use_hmi

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
