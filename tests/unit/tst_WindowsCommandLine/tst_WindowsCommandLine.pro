QT = core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = tst_WindowsCommandLine
SOURCES = tst_WindowsCommandLine.cpp
win32: LIBS += -lshell32
