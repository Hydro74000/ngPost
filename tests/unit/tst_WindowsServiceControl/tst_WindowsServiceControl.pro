QT = core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = tst_WindowsServiceControl
SOURCES = tst_WindowsServiceControl.cpp ../../../src/vpn/WindowsServiceControl.cpp
win32: LIBS += -ladvapi32
