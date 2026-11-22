# Queue manager drawer, on the in-memory store and the fake executor. Builds
# only the queue model and its widgets (QtWidgets), not the whole core.
TARGET = tst_QueuePanel
include(../../unit/queue_tests.pri)
QT += widgets
SOURCES += tst_QueuePanel.cpp \
           ../../../src/hmi/QueueIcons.cpp \
           ../../../src/hmi/QueueItemDelegate.cpp \
           ../../../src/hmi/QueuePanel.cpp \
           ../../../src/hmi/QueueTableModel.cpp
HEADERS += ../../../src/hmi/QueueIcons.h \
           ../../../src/hmi/QueueItemDelegate.h \
           ../../../src/hmi/QueuePanel.h \
           ../../../src/hmi/QueueTableModel.h
