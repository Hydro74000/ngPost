# queue_tests.pri — shared settings of the queue unit tests.
#
# They build the queue model alone (src/queue/queue.pri) rather than the
# whole core: QtCore only, a few seconds per binary.

QT = core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

INCLUDEPATH += $$PWD/../../src $$PWD/../common
include($$PWD/../../src/queue/queue.pri)

SOURCES += $$PWD/../../src/par2/Par2Settings.cpp \
           $$PWD/../../src/tools/ExternalToolResolver.cpp
HEADERS += $$PWD/../../src/par2/Par2Settings.h \
           $$PWD/../common/QueueTestHelpers.h

!msvc: QMAKE_CXXFLAGS += -Wshadow
# Same strictness as the core (see ngPost_core.pri): our code under -Werror,
# Qt's headers as system headers so a warning inside them does not count.
ngpost_werror {
    !msvc {
        QUEUE_TEST_SYSTEM_HEADERS = $$[QT_INSTALL_HEADERS] $$files($$[QT_INSTALL_HEADERS]/Qt*)
        QMAKE_CXXFLAGS += -Werror
        for(dir, QUEUE_TEST_SYSTEM_HEADERS): QMAKE_CXXFLAGS += -isystem $$shell_quote($$dir)
        macx: QMAKE_CXXFLAGS += -iframework $$shell_quote($$[QT_INSTALL_LIBS])
    }
    msvc: QMAKE_CXXFLAGS += /WX /external:anglebrackets /external:W0
}
