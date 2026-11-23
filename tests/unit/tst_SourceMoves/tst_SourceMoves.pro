# The journaled moves of an obfuscation (queue manager, lot C5.7) against a
# real history database, closed and reopened, and real files.
TARGET = tst_SourceMoves
include(../queue_tests.pri)
QT += sql
DEFINES += APP_VERSION=\\\"queue-test\\\" NGPOST_TESTING
SOURCES += tst_SourceMoves.cpp \
           ../../../src/history/PostHistoryStore.cpp \
           ../../../src/history/PostHistoryStoreQueue.cpp \
           ../../../src/history/PostHistoryStorePreparation.cpp \
           ../../../src/history/HistoryQueueStore.cpp \
           ../../../src/tools/SourceMoves.cpp \
           ../../../src/utils/NativeFileIdentity.cpp \
           ../../../src/utils/NoReplaceRename.cpp
HEADERS += ../../../src/history/PostHistoryStore.h \
           ../../../src/history/HistoryQueueStore.h \
           ../../../src/tools/SourceMoves.h \
           ../../../src/utils/NativeFileIdentity.h \
           ../../../src/utils/NoReplaceRename.h
