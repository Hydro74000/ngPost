// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// J0 milestone (plan_queue.md §11): the real queue model and scheduler on
// an in-memory store and a simulated executor, so the queue manager can be
// handled without posting, renaming, compressing or deleting anything.
// Enabled by the development variable NGPOST_QUEUE_PREVIEW only.
//
//========================================================================

#ifndef QUEUE_QUEUEPREVIEW_H
#define QUEUE_QUEUEPREVIEW_H

#include "queue/FakeQueueExecutor.h"
#include "queue/InMemoryQueueStore.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"

#include <QObject>
#include <QStringList>

namespace queue
{

class QueuePreview : public QObject
{
    Q_OBJECT

public:
    explicit QueuePreview(QObject *parent = nullptr);

    static bool requested();

    PostQueue *queue() { return &_queue; }
    QueueExecutor *executor() { return &_executor; }

    //! A handful of posts in every state worth looking at.
    void addDemoPosts();
    //! One simulated post per path; only sizes are read from the disk.
    void addPaths(const QStringList &paths);

private:
    void add(const QString &name, bool compress, bool par2, qint64 bytes, Origin origin, int tab);

    InMemoryQueueStore _store;
    PostQueue _queue;
    FakeQueueExecutor _executor;
    QueueScheduler _scheduler;
};

} // namespace queue

#endif // QUEUE_QUEUEPREVIEW_H
