QT = core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = tst_SecretMasker
SOURCES = tst_SecretMasker.cpp ../../../src/utils/SecretMasker.cpp
