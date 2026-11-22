// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// ServiceQueueStore: see HistoryQueueStore.h. Kept apart so that tests of the
// SQL side link without the history worker.

#include "history/HistoryQueueStore.h"

#include "history/PostHistoryService.h"

using namespace queue;

QueueCommit ServiceQueueStore::commit(const QueueTransaction &transaction)
{
    return _service->commitQueueTransaction(transaction);
}

QList<QueueEntry> ServiceQueueStore::loadEntries(const QString &queueId) const
{
    return _service->loadQueueEntries(queueId);
}

QList<qint64> ServiceQueueStore::loadOrder(const QString &queueId, Domain domain) const
{
    return _service->loadQueueOrder(queueId, domain);
}

std::optional<PostNowIntent> ServiceQueueStore::loadIntent(const QString &queueId) const
{
    return _service->loadQueueIntent(queueId);
}
