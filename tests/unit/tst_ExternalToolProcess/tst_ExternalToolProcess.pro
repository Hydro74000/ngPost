TARGET = tst_ExternalToolProcess
include(../queue_tests.pri)
DEFINES += NGPOST_TESTING
SOURCES += tst_ExternalToolProcess.cpp \
           ../../../src/tools/ExternalToolProcess.cpp \
           ../../../src/tools/ToolSupervisor.cpp
HEADERS += ../../../src/tools/ExternalToolProcess.h \
           ../../../src/tools/ToolSupervisor.h
