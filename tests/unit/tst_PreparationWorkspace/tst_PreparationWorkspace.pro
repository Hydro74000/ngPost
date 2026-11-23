TARGET = tst_PreparationWorkspace
include(../queue_tests.pri)
SOURCES += tst_PreparationWorkspace.cpp \
           ../../../src/utils/NativeFileIdentity.cpp \
           ../../../src/tools/PreparationWorkspace.cpp
HEADERS += ../../../src/utils/NativeFileIdentity.h \
           ../../../src/tools/PreparationWorkspace.h
