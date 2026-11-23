TARGET = tst_ExternalToolProcess
include(../queue_tests.pri)
DEFINES += NGPOST_TESTING
QT += network
win32: LIBS += -lshell32
SOURCES += tst_ExternalToolProcess.cpp \
           ../../../src/tools/ExternalToolProcess.cpp \
           ../../../src/tools/ToolSupervisor.cpp
HEADERS += ../../../src/tools/ExternalToolProcess.h \
           ../../../src/tools/ToolSupervisor.h
