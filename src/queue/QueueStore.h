// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Durable side of the queue (plan_queue.md §5.5), as a unit of work: a
// QueueTransaction describes everything one command changes and the store
// applies it entirely or not at all. PostQueue publishes a change in memory
// only after commit() succeeded (invariant 8).
//
// The SQLite implementation (PostHistoryStoreQueue, lot C2) maps one
// transaction onto one SQL transaction. InMemoryQueueStore serves the tests
// and the internal J0 milestone.
//
//========================================================================

#ifndef QUEUE_QUEUESTORE_H
#define QUEUE_QUEUESTORE_H

#include "queue/QueueEntry.h"

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace queue
{

struct QueueTransaction
{
    struct Order
    {
        QString queueId;
        Domain domain = Domain::Session;
        QList<qint64> ids;
        quint64 revision = 0;
    };

    //! Entries written. A post id <= 0 is a placeholder for a new admission; the
    //! store assigns the real id and the first run, and the same placeholder may
    //! appear in orders.
    QList<QueueEntry> upserts;
    //! Entries leaving the queue (the post itself stays in the history).
    QList<qint64> removals;
    //! Post ids whose current run ends as interrupted and gets a successor run.
    QList<qint64> renewRuns;
    QList<Order> orders;

    std::optional<PostNowIntent> setIntent;
    QString dropIntentId;

    bool isEmpty() const
    {
        return upserts.isEmpty() && removals.isEmpty() && renewRuns.isEmpty() && orders.isEmpty()
            && !setIntent && dropIntentId.isEmpty();
    }
};

struct QueueCommit
{
    QString error;
    //! placeholder id -> assigned post id, for admissions
    QHash<qint64, qint64> assignedIds;
    struct Run
    {
        QString runId;
        int runNo = 0;
    };
    //! post id -> run now attached to its entry (admissions and renewals)
    QHash<qint64, Run> runs;

    bool ok() const { return error.isEmpty(); }
};

class QueueStore
{
public:
    virtual ~QueueStore() = default;

    virtual QueueCommit commit(const QueueTransaction &transaction) = 0;

    // Restoration (lot C2): what a claimed queue holds.
    virtual QList<QueueEntry> loadEntries(const QString &queueId) const = 0;
    virtual QList<qint64> loadOrder(const QString &queueId, Domain domain) const = 0;
    virtual std::optional<PostNowIntent> loadIntent(const QString &queueId) const = 0;
};

} // namespace queue

#endif // QUEUE_QUEUESTORE_H
