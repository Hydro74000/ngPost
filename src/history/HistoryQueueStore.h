// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// queue::QueueStore on the history database (plan_queue.md §5.5).
//
//   HistoryQueueStore  talks to a PostHistoryStore directly: tests, and code
//                      already running on the history worker's thread.
//   ServiceQueueStore  goes through PostHistoryService, which runs the same
//                      calls on its SQLite worker and waits for the answer,
//                      like every other history write of the application.
//
//========================================================================

#ifndef HISTORY_HISTORYQUEUESTORE_H
#define HISTORY_HISTORYQUEUESTORE_H

#include "queue/QueueStore.h"

class PostHistoryStore;
class PostHistoryService;

class HistoryQueueStore : public queue::QueueStore
{
public:
    explicit HistoryQueueStore(PostHistoryStore *store)
        : _store(store)
    {
    }

    queue::QueueCommit commit(const queue::QueueTransaction &transaction) override;
    QList<queue::QueueEntry> loadEntries(const QString &queueId) const override;
    QList<qint64> loadOrder(const QString &queueId, queue::Domain domain) const override;
    std::optional<queue::PostNowIntent> loadIntent(const QString &queueId) const override;

private:
    PostHistoryStore *_store;
};

class ServiceQueueStore : public queue::QueueStore
{
public:
    explicit ServiceQueueStore(PostHistoryService *service)
        : _service(service)
    {
    }

    queue::QueueCommit commit(const queue::QueueTransaction &transaction) override;
    QList<queue::QueueEntry> loadEntries(const QString &queueId) const override;
    QList<qint64> loadOrder(const QString &queueId, queue::Domain domain) const override;
    std::optional<queue::PostNowIntent> loadIntent(const QString &queueId) const override;

private:
    PostHistoryService *_service;
};

#endif // HISTORY_HISTORYQUEUESTORE_H
