# queue.pri — the queue manager model (plan_queue.md §3).
#
# Included by ngPost_core.pri for the app and by the queue unit tests, which
# build only these sources (plus par2/Par2Settings.cpp and
# tools/ExternalToolResolver.cpp, already in the core list) instead of the
# whole core.

SOURCES += \
    $$PWD/../PostingJobOptionsCodec.cpp \
    $$PWD/FakeQueueExecutor.cpp \
    $$PWD/InMemoryQueueStore.cpp \
    $$PWD/PostQueue.cpp \
    $$PWD/QueueCapabilities.cpp \
    $$PWD/QueuePreview.cpp \
    $$PWD/QueueScheduler.cpp \
    $$PWD/QueueTypes.cpp

HEADERS += \
    $$PWD/../PostingJobOptionsCodec.h \
    $$PWD/FakeQueueExecutor.h \
    $$PWD/InMemoryQueueStore.h \
    $$PWD/PostQueue.h \
    $$PWD/QueueCapabilities.h \
    $$PWD/QueueEntry.h \
    $$PWD/QueueExecutor.h \
    $$PWD/QueuePreview.h \
    $$PWD/QueueScheduler.h \
    $$PWD/QueueStore.h \
    $$PWD/QueueTypes.h
