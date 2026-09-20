# qmake config_snapshot.pro SOURCE_ROOT=/absolute/path/to/source/tree
isEmpty(SOURCE_ROOT): SOURCE_ROOT = $$absolute_path($$PWD/../..)
include($$SOURCE_ROOT/tests/common/common.pri)
CONFIG += console
TARGET = config_snapshot
SOURCES += $$PWD/config_snapshot.cpp
# saveConfig() translates its comments; use the application's real catalogues.
RESOURCES += $$SOURCE_ROOT/src/resources/resources.qrc
