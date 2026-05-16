# tests/common/common.pri — shared settings for all QTest binaries.
#
# Each test .pro under tests/unit/, tests/gui/, tests/vpn/ includes this file
# and adds its own SOURCES on top. Tests recompile the ngPost core sources via
# the inclusion of ngPost_core.pri (we don't link a static library — see the
# Phase 1 note in tests/README.md). That is slower at link time but avoids the
# qmake SUBDIRS / Makefile-collision dance, which behaves differently on
# Windows MSVC and macOS clang.

QT       += testlib
CONFIG   += qt warn_on testcase c++17
CONFIG   -= app_bundle
TEMPLATE  = app

# Pull in the same source list, Qt modules, DEFINES and platform link flags
# as the app. main.cpp is NOT in ngPost_core.pri, so we don't get a duplicate
# main() — QTEST_MAIN(TestClass) at the bottom of each tst_*.cpp file fills
# that role.
include($$PWD/../../src/ngPost_core.pri)

# Test-only build flag. Code under #ifdef NGPOST_TESTING exposes private
# members to NgPostTestAccess (see TestEnv.h).
DEFINES += NGPOST_TESTING

# Shared test helpers
SOURCES += $$PWD/TestEnv.cpp \
           $$PWD/MockNntpServer.cpp \
           $$PWD/MockDnsServer.cpp
HEADERS += $$PWD/TestEnv.h \
           $$PWD/Fixtures.h \
           $$PWD/MockNntpServer.h \
           $$PWD/MockDnsServer.h
INCLUDEPATH += $$PWD

# Repo root and fixtures dir as absolute paths, injected at compile time so
# code in tests/common/ doesn't have to deal with brittle __FILE__-relative
# resolution. NGPOST_TESTS_ROOT points at <repo>/tests; helpers append
# whatever subdirectory they need.
NGPOST_TESTS_ROOT_ABS = $$absolute_path($$PWD/..)
DEFINES += NGPOST_TESTS_ROOT=\\\"$$NGPOST_TESTS_ROOT_ABS\\\"
