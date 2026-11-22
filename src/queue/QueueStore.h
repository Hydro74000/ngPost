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
#include <QSet>
#include <QString>

#include <optional>

namespace queue
{

//! How the current run of an entry ends when it leaves with a known outcome.
struct RunOutcome
{
    QString result;   //!< post_runs.result: "failed", ...
    QString endCause; //!< post_runs.end_cause: "technical", ...
    QString diagnosticJson;
};

//! F1 (plan_queue.md §6.2): the durable result of one attempt, recorded once.
//! The entry stays, its run waiting for its NZB (finalization 'nzb_pending').
struct AttemptResult
{
    qint64 postId = 0;
    QString runId;
    QString result;   //!< success, partial, failed or cancelled
    QString endCause; //!< normal, user_stop or technical
    QString avgSpeed;
    qint64 activeMs = 0; //!< transfer time of this attempt, added once
};

//! The end of a finalization: F3 once the NZB is published, or an explicit
//! abandon (facts kept, no deliverable). The entry leaves the queue; the post
//! keeps its owner until its local work is over (see releases).
struct FinalizationStep
{
    enum class Kind {
        Complete,
        Abandon
    };
    qint64 postId = 0;
    QString runId;
    Kind kind = Kind::Complete;
};

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
    //! Posts already in the history entering the queue again (a resume): the
    //! store attaches their next run.
    QList<QueueEntry> existingAdmissions;
    //! Entries leaving the queue (the post itself stays in the history).
    QList<qint64> removals;
    //! For some of the removals: the outcome their current run ends with. A
    //! post that never transferred takes it as its status (a resume keeps its own).
    QHash<qint64, RunOutcome> outcomes;

    //! Preconditions: post id -> the run the command was issued for, owned by
    //! this session. Anything else fails the whole transaction.
    QHash<qint64, QString> expectedRuns;
    //! Preconditions: post id -> the recipe revision the command saw.
    QHash<qint64, quint64> expectedRevisions;

    //! What a transaction may not combine for one post: a renewal with its F1,
    //! its finalization or its removal, a finalization with a removal. And what
    //! every operation on a run must state. Empty when the transaction is sound.
    QString shapeError() const
    {
        QSet<qint64> renewed(renewRuns.cbegin(), renewRuns.cend());
        QSet<qint64> finished;
        for (const FinalizationStep &step : finalizations)
            finished.insert(step.postId);
        for (const AttemptResult &attempt : results)
            if (renewed.contains(attempt.postId))
                return QStringLiteral("conflicting_operations");
        for (qint64 id : removals)
            if (renewed.contains(id) || finished.contains(id))
                return QStringLiteral("conflicting_operations");
        if (finished.intersects(renewed))
            return QStringLiteral("conflicting_operations");
        QSet<qint64> stated = renewed + finished;
        for (const AttemptResult &attempt : results)
            stated.insert(attempt.postId);
        for (qint64 id : std::as_const(stated))
            if (!expectedRevisions.contains(id)
                || (renewed.contains(id) && !expectedRuns.contains(id)))
                return QStringLiteral("missing_precondition");
        return {};
    }
    QList<AttemptResult> results;              //!< F1
    QList<FinalizationStep> finalizations;     //!< F3 or abandon: these entries leave
    QHash<qint64, qint64> interruptedActiveMs; //!< renewRuns: time of the run kept
    QList<qint64> releases;                    //!< posts out of the queue whose local work is over
    //! Post ids whose current run ends as interrupted and gets a successor run.
    QList<qint64> renewRuns;
    QList<Order> orders;

    std::optional<PostNowIntent> setIntent;
    QString dropIntentId;

    bool isEmpty() const
    {
        return upserts.isEmpty() && existingAdmissions.isEmpty() && removals.isEmpty()
            && renewRuns.isEmpty() && orders.isEmpty() && !setIntent && dropIntentId.isEmpty()
            && results.isEmpty() && finalizations.isEmpty() && releases.isEmpty();
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
    //! post id -> run now attached to its entry (admissions, resumes and renewals)
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
