// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "history/HistoryQueueStore.h"

#include "history/PostHistoryStore.h"

using namespace queue;

QueueCommit HistoryQueueStore::commit(const QueueTransaction &transaction)
{
    return _store->commitQueueTransaction(transaction);
}

QList<QueueEntry> HistoryQueueStore::loadEntries(const QString &queueId) const
{
    return _store->loadQueueEntries(queueId);
}

QList<qint64> HistoryQueueStore::loadOrder(const QString &queueId, Domain domain) const
{
    return _store->loadQueueOrder(queueId, domain);
}

std::optional<PostNowIntent> HistoryQueueStore::loadIntent(const QString &queueId) const
{
    return _store->loadQueueIntent(queueId);
}
