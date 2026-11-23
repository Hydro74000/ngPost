# The SQL side of the queue manager (schema v6) on a temporary database,
# without the history worker nor the rest of the core.
TARGET = tst_QueueStoreSqlite
include(../queue_tests.pri)
QT += sql
DEFINES += APP_VERSION=\\\"queue-test\\\"
SOURCES += tst_QueueStoreSqlite.cpp \
           ../../../src/history/PostHistoryStore.cpp \
           ../../../src/history/PostHistoryStoreQueue.cpp \
           ../../../src/history/PostHistoryStorePreparation.cpp \
           ../../../src/history/HistoryQueueStore.cpp
HEADERS += ../../../src/history/PostHistoryStore.h \
           ../../../src/history/HistoryQueueStore.h
